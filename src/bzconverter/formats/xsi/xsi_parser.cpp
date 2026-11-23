#include "bzconverter/formats/xsi/xsi_parser.hpp"
#include "bzconverter/core/sha256.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace bz {

namespace {

struct XsiToken {
    enum class Kind { Word, String, LBrace, RBrace, Eof };
    Kind kind = Kind::Eof;
    std::string text;
    int line = 1;
};

class XsiLexer {
public:
    explicit XsiLexer(const uint8_t* data, size_t size) : data_(data), size_(size) {
        if (size_ >= 3 && data_[0] == 0xEF && data_[1] == 0xBB && data_[2] == 0xBF) pos_ = 3;
    }

    XsiToken peek() {
        if (!has_peek_) {
            peek_tok_ = next_raw();
            has_peek_ = true;
        }
        return peek_tok_;
    }

    XsiToken next() {
        if (has_peek_) {
            has_peek_ = false;
            return peek_tok_;
        }
        return next_raw();
    }

    int line() const { return line_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    int line_ = 1;
    bool has_peek_ = false;
    XsiToken peek_tok_;

    void skip_ws_and_comments() {
        while (pos_ < size_) {
            char c = static_cast<char>(data_[pos_]);
            if (c == '\n') { ++line_; ++pos_; continue; }
            if (c == ' ' || c == '\t' || c == '\r' || c == ';' || c == ',') { ++pos_; continue; }
            if (c == '#') {
                while (pos_ < size_ && data_[pos_] != '\n') ++pos_;
                continue;
            }
            if (c == '/' && pos_ + 1 < size_ && data_[pos_ + 1] == '/') {
                pos_ += 2;
                while (pos_ < size_ && data_[pos_] != '\n') ++pos_;
                continue;
            }
            if (c == '/' && pos_ + 1 < size_ && data_[pos_ + 1] == '*') {
                pos_ += 2;
                while (pos_ + 1 < size_ && !(data_[pos_] == '*' && data_[pos_ + 1] == '/')) {
                    if (data_[pos_] == '\n') ++line_;
                    ++pos_;
                }
                if (pos_ + 1 < size_) pos_ += 2;
                continue;
            }
            break;
        }
    }

    XsiToken next_raw() {
        skip_ws_and_comments();
        if (pos_ >= size_) return {XsiToken::Kind::Eof, "", line_};
        char c = static_cast<char>(data_[pos_]);
        int tok_line = line_;
        if (c == '{') { ++pos_; return {XsiToken::Kind::LBrace, "{", tok_line}; }
        if (c == '}') { ++pos_; return {XsiToken::Kind::RBrace, "}", tok_line}; }
        if (c == '"' || c == '\'') {
            char quote = c;
            ++pos_;
            std::string s;
            while (pos_ < size_) {
                char ch = static_cast<char>(data_[pos_++]);
                if (ch == '\r') continue;
                if (ch == '\n') { ++line_; continue; }
                if (ch == '\\' && pos_ < size_) {
                    char nx = static_cast<char>(data_[pos_]);
                    if (nx == quote || nx == '\\') { s.push_back(nx); ++pos_; continue; }
                }
                if (ch == quote) break;
                s.push_back(ch);
            }
            return {XsiToken::Kind::String, std::move(s), tok_line};
        }
        std::string w;
        while (pos_ < size_) {
            char ch = static_cast<char>(data_[pos_]);
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
                ch == ';' || ch == ',' || ch == '{' || ch == '}' || ch == '"' || ch == '\'') break;
            if (ch == '#' || (ch == '/' && pos_ + 1 < size_ && (data_[pos_+1] == '/' || data_[pos_+1] == '*'))) break;
            w.push_back(ch);
            ++pos_;
        }
        return {XsiToken::Kind::Word, std::move(w), tok_line};
    }
};

std::string clean_xsi_name(std::string name) {
    if (name.size() >= 2 && name.front() == '{' && name.back() == '}')
        name = name.substr(1, name.size() - 2);
    std::string lo = to_lower(name);
    if (lo.rfind("frm-", 0) == 0) name = name.substr(4);
    else if (lo.rfind("anim-", 0) == 0) name = name.substr(5);
    if (name.empty()) name = "unnamed";
    return name;
}

void apply_bz_frame_flags(Node& node) {
    std::string lo = to_lower(node.name);
    auto dpos = lo.rfind("__");
    std::string flags;
    if (dpos != std::string::npos && dpos + 2 < lo.size()) flags = lo.substr(dpos + 2);

    std::string prefix = lo;
    auto upos = lo.find('_');
    if (upos != std::string::npos) prefix = lo.substr(0, upos);

    if (prefix == "hp") {
        node.is_hardpoint = true;
        flags += "h";
    } else if (prefix == "flame") {
        flags += "IFe";
    } else if (prefix == "tractor") {
        flags += "I";
    }
    node.bz_flags = flags;
    bool ignore_hidden = (flags.find('I') != std::string::npos);
    if (!ignore_hidden) {
        if (flags.find('h') != std::string::npos) node.is_hidden = true;
        if (flags.find('c') != std::string::npos) node.is_collision = true;
    }
}

struct RawEnvelope {
    std::string mesh_frame;
    std::string bone_frame;
    std::vector<std::pair<int, float>> weights; // (vertex_index, weight 0..100)
};

struct RawAnimKey {
    int key_type = 0; // 0=quat WXYZ, 1=scale, 2=trans, 3=euler
    std::vector<std::pair<float, std::vector<float>>> keys;
};

struct RawAnim {
    std::string target_frame;
    std::vector<RawAnimKey> keys;
};

struct RawXsiMesh {
    std::string name;
    std::vector<Vec3> positions;
    std::vector<std::vector<int>> faces;
    std::vector<Vec3> normals;
    std::vector<std::vector<int>> normal_faces; // aligned by face index
    std::vector<Vec2> uvs;
    std::vector<std::vector<int>> uv_faces;
    std::vector<Vec4> vcols;
    std::vector<std::vector<int>> vcol_faces;
    std::vector<int> face_mat_indices;
    std::vector<int> local_to_scene_mat;
};

// Ear-clipping / convex fan triangulation preserving exact polygon corner order
// Returns index triples into [0 .. n-1] corner indices of polygon.
std::vector<std::array<int, 3>> triangulate_polygon(const std::vector<Vec3>& verts, const std::vector<int>& poly) {
    int n = static_cast<int>(poly.size());
    std::vector<std::array<int, 3>> tris;
    if (n < 3) return tris;
    if (n == 3) { tris.push_back({0, 1, 2}); return tris; }
    if (n == 4) {
        // Choose diagonal that avoids folded/degenerate quad
        const Vec3& p0 = verts[poly[0]];
        const Vec3& p1 = verts[poly[1]];
        const Vec3& p2 = verts[poly[2]];
        const Vec3& p3 = verts[poly[3]];
        float a1 = (p1 - p0).cross(p2 - p0).length() + (p2 - p0).cross(p3 - p0).length();
        float a2 = (p1 - p0).cross(p3 - p0).length() + (p2 - p1).cross(p3 - p1).length();
        if (a2 > a1 * 1.001f) {
            tris.push_back({0, 1, 3});
            tris.push_back({1, 2, 3});
        } else {
            tris.push_back({0, 1, 2});
            tris.push_back({0, 2, 3});
        }
        return tris;
    }

    // Newell polygon normal for 2D projection
    Vec3 normal{0, 0, 0};
    for (int i = 0; i < n; ++i) {
        const Vec3& a = verts[poly[i]];
        const Vec3& b = verts[poly[(i + 1) % n]];
        normal.x += (a.y - b.y) * (a.z + b.z);
        normal.y += (a.z - b.z) * (a.x + b.x);
        normal.z += (a.x - b.x) * (a.y + b.y);
    }
    if (normal.length() < 1e-10f) {
        for (int i = 1; i + 1 < n; ++i) tris.push_back({0, i, i + 1});
        return tris;
    }
    normal = normal.normalized();

    // Project onto dominant 2D plane
    int drop = 2;
    float ax = std::fabs(normal.x), ay = std::fabs(normal.y), az = std::fabs(normal.z);
    if (ax >= ay && ax >= az) drop = 0;
    else if (ay >= ax && ay >= az) drop = 1;

    auto proj2d = [&](int corner) -> Vec2 {
        const Vec3& p = verts[poly[corner]];
        if (drop == 0) return {p.y, p.z};
        if (drop == 1) return {p.z, p.x};
        return {p.x, p.y};
    };

    // Determine winding sign in 2D
    float area2 = 0.0f;
    for (int i = 0; i < n; ++i) {
        Vec2 a = proj2d(i), b = proj2d((i + 1) % n);
        area2 += (a.x * b.y - b.x * a.y);
    }
    float sign = (area2 >= 0.0f) ? 1.0f : -1.0f;

    auto cross2 = [](Vec2 a, Vec2 b, Vec2 c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    auto pt_in_tri = [&](Vec2 p, Vec2 a, Vec2 b, Vec2 c) {
        float c1 = cross2(a, b, p) * sign;
        float c2 = cross2(b, c, p) * sign;
        float c3 = cross2(c, a, p) * sign;
        return (c1 >= -1e-6f) && (c2 >= -1e-6f) && (c3 >= -1e-6f);
    };

    std::vector<int> ring(n);
    for (int i = 0; i < n; ++i) ring[i] = i;

    int guard = n * n + 16;
    while (ring.size() > 3 && guard-- > 0) {
        bool clipped = false;
        int m = static_cast<int>(ring.size());
        for (int i = 0; i < m; ++i) {
            int i0 = ring[(i + m - 1) % m];
            int i1 = ring[i];
            int i2 = ring[(i + 1) % m];
            Vec2 a = proj2d(i0), b = proj2d(i1), c = proj2d(i2);
            if (cross2(a, b, c) * sign <= 1e-8f) continue;
            bool inside = false;
            for (int j = 0; j < m; ++j) {
                int ij = ring[j];
                if (ij == i0 || ij == i1 || ij == i2) continue;
                if (pt_in_tri(proj2d(ij), a, b, c)) { inside = true; break; }
            }
            if (!inside) {
                tris.push_back({i0, i1, i2});
                ring.erase(ring.begin() + i);
                clipped = true;
                break;
            }
        }
        if (!clipped) break;
    }
    if (ring.size() == 3) {
        tris.push_back({ring[0], ring[1], ring[2]});
    } else if (ring.size() > 3) {
        // Fallback fan for self-intersecting polygons
        for (size_t i = 1; i + 1 < ring.size(); ++i)
            tris.push_back({ring[0], ring[i], ring[i + 1]});
    }
    return tris;
}

class XsiParserImpl {
public:
    XsiParserImpl(XsiLexer& lex, Scene& scene, const XsiParseOptions& opts)
        : lex_(lex), scene_(scene), opts_(opts) {}

    bool parse(std::string& err) {
        // Header: xsi 0101txt 0032
        auto t0 = lex_.next();
        auto t1 = lex_.next();
        auto t2 = lex_.next();
        if (to_lower(t0.text) != "xsi" || t1.kind != XsiToken::Kind::Word || t2.kind != XsiToken::Kind::Word) {
            err = "Invalid XSI header (expected 'xsi 0101txt 0032')";
            return false;
        }
        std::string vstr = to_lower(t1.text);
        if (vstr.find("0101txt") != std::string::npos) scene_.source_format = SourceFormat::XSI_Text_11;
        else if (vstr.find("0103txt") != std::string::npos) scene_.source_format = SourceFormat::XSI_Text_13;
        else {
            err = "Unsupported XSI version token: " + t1.text;
            return false;
        }

        while (true) {
            auto tok = lex_.peek();
            if (tok.kind == XsiToken::Kind::Eof) break;
            if (tok.kind == XsiToken::Kind::RBrace) {
                lex_.next();
                scene_.add_diag(DiagSeverity::Warning, "XSI_EXTRA_RBRACE",
                    "Stray '}' at root level line " + std::to_string(tok.line));
                continue;
            }
            std::string btype, bname;
            if (!read_block_header(btype, bname, err)) return false;
            std::string lo = to_lower(btype);
            if (lo == "si_coordinatesystem") {
                read_coord_system();
            } else if (lo == "si_angle") {
                read_angle();
            } else if (lo == "frame" || lo == "si_frame") {
                if (!read_frame(-1, bname, err)) return false;
            } else if (lo == "animationset" || lo == "si_animationset") {
                if (opts_.include_animations) read_animation_set();
                else skip_block_body();
            } else if (lo == "si_envelopelist" || lo == "envelopelist") {
                if (opts_.include_envelopes) read_envelope_list();
                else skip_block_body();
            } else if (lo == "si_camera" || lo == "si_light" || lo == "si_ambience" || lo == "si_fog") {
                skip_block_body();
            } else {
                scene_.add_diag(DiagSeverity::Info, "XSI_SKIPPED_BLOCK",
                    "Skipped top-level block '" + btype + "' at line " + std::to_string(tok.line));
                skip_block_body();
            }
        }

        finalize_meshes();
        finalize_envelopes();
        finalize_animations();
        scene_.update_world_matrices();
        scene_.normalize_skin_weights();
        scene_.ensure_inverse_bind_matrices();

        if (scene_.nodes.empty()) {
            if (!scene_.animations.empty()) {
                scene_.is_animation_only = true;
                scene_.add_diag(DiagSeverity::Info, "XSI_ANIMATION_ONLY_ASSET",
                                "XSI file contains " + std::to_string(scene_.animations.size()) +
                                " animation clip(s) with no Frame nodes (animation-only companion asset)");
                return true;
            }
            err = "XSI file contained no Frame nodes";
            return false;
        }
        return true;
    }

private:
    XsiLexer& lex_;
    Scene& scene_;
    XsiParseOptions opts_;
    int angle_units_ = 0; // 0 = degrees, 1 = radians
    std::unordered_map<std::string, int> frame_name_map_;
    std::vector<std::pair<int, RawXsiMesh>> raw_meshes_; // (node_index, raw_mesh)
    std::vector<RawEnvelope> raw_envelopes_;
    std::vector<RawAnim> raw_anims_;
    bool saw_anim_param_key_ = false;

    bool read_block_header(std::string& btype, std::string& bname, std::string& err) {
        auto t = lex_.next();
        if (t.kind != XsiToken::Kind::Word) {
            err = "Expected block keyword at line " + std::to_string(t.line) + ", got '" + t.text + "'";
            return false;
        }
        btype = t.text;
        bname.clear();
        while (true) {
            auto p = lex_.peek();
            if (p.kind == XsiToken::Kind::LBrace) { lex_.next(); return true; }
            if (p.kind == XsiToken::Kind::Eof) {
                err = "Unexpected EOF waiting for '{' after '" + btype + "' at line " + std::to_string(t.line);
                return false;
            }
            if (bname.empty()) bname = p.text;
            lex_.next();
        }
    }

    void skip_block_body() {
        int depth = 1;
        while (depth > 0) {
            auto t = lex_.next();
            if (t.kind == XsiToken::Kind::Eof) break;
            if (t.kind == XsiToken::Kind::LBrace) ++depth;
            else if (t.kind == XsiToken::Kind::RBrace) --depth;
        }
    }

    bool read_float(float& out) {
        auto t = lex_.next();
        if (t.kind != XsiToken::Kind::Word) return false;
        char* ep = nullptr;
        out = std::strtof(t.text.c_str(), &ep);
        return ep != t.text.c_str() && std::isfinite(out);
    }

    bool read_int(int& out) {
        auto t = lex_.next();
        if (t.kind != XsiToken::Kind::Word) return false;
        char* ep = nullptr;
        double d = std::strtod(t.text.c_str(), &ep);
        if (ep == t.text.c_str() || !std::isfinite(d)) return false;
        out = static_cast<int>(d);
        return true;
    }

    void read_coord_system() {
        std::vector<int> vals;
        while (lex_.peek().kind == XsiToken::Kind::Word) {
            int v = 0;
            if (read_int(v)) vals.push_back(v);
        }
        skip_block_body();
        if (vals.size() >= 6 && !(vals[0] == 1 && vals[1] == 0 && vals[2] == 1 && vals[3] == 0 && vals[4] == 2 && vals[5] == 5)) {
            scene_.add_diag(DiagSeverity::Warning, "XSI_NONSTANDARD_COORD",
                "Non-standard SI_CoordinateSystem values (" + std::to_string(vals[0]) + "," +
                std::to_string(vals[1]) + "," + std::to_string(vals[2]) + "," +
                std::to_string(vals[3]) + "," + std::to_string(vals[4]) + "," + std::to_string(vals[5]) + ")");
        }
    }

    void read_angle() {
        int v = 0;
        if (lex_.peek().kind == XsiToken::Kind::Word && read_int(v)) angle_units_ = v;
        skip_block_body();
    }

    bool read_matrix(Mat4& mat) {
        for (int i = 0; i < 16; ++i) {
            if (!read_float(mat.m[i])) {
                skip_block_body();
                return false;
            }
        }
        skip_block_body();
        return true;
    }

    bool read_frame(int parent_idx, const std::string& raw_name, std::string& err) {
        std::string name = clean_xsi_name(raw_name);
        std::string unique = name;
        while (frame_name_map_.count(unique)) unique += "_";
        if (unique != name) {
            scene_.add_diag(DiagSeverity::Warning, "XSI_DUPLICATE_FRAME",
                "Duplicate frame name '" + name + "' renamed to '" + unique + "'");
        }

        int node_idx = static_cast<int>(scene_.nodes.size());
        Node node;
        node.name = unique;
        node.raw_name = raw_name;
        node.parent_index = parent_idx;
        apply_bz_frame_flags(node);
        scene_.nodes.push_back(std::move(node));
        frame_name_map_[unique] = node_idx;
        if (parent_idx >= 0) scene_.nodes[parent_idx].children.push_back(node_idx);
        else scene_.root_nodes.push_back(node_idx);

        while (true) {
            auto pk = lex_.peek();
            if (pk.kind == XsiToken::Kind::Eof) {
                scene_.add_diag(DiagSeverity::Warning, "XSI_UNCLOSED_FRAME",
                    "Unclosed Frame '" + unique + "' reached EOF");
                break;
            }
            if (pk.kind == XsiToken::Kind::RBrace) {
                lex_.next();
                break;
            }
            std::string btype, bname;
            if (!read_block_header(btype, bname, err)) return false;
            std::string lo = to_lower(btype);
            if (lo == "frametransformmatrix" || lo == "si_frametransformmatrix") {
                Mat4 m;
                if (read_matrix(m)) scene_.nodes[node_idx].local_matrix = m;
            } else if (lo == "si_framebaseposematrix" || lo == "framebaseposematrix") {
                Mat4 m;
                if (read_matrix(m)) {
                    scene_.nodes[node_idx].base_pose_matrix = m;
                    scene_.nodes[node_idx].has_base_pose = true;
                }
            } else if (lo == "mesh" || lo == "si_mesh") {
                RawXsiMesh rm;
                rm.name = bname.empty() ? unique : clean_xsi_name(bname);
                if (!read_mesh(rm, scene_.nodes[node_idx], err)) return false;
                raw_meshes_.emplace_back(node_idx, std::move(rm));
            } else if (lo == "frame" || lo == "si_frame") {
                if (!read_frame(node_idx, bname, err)) return false;
            } else if (lo == "animationset" || lo == "si_animationset") {
                // Handle Battlezone files where missing brace nests AnimationSet inside last Frame
                if (opts_.include_animations) read_animation_set();
                else skip_block_body();
            } else if (lo == "si_envelopelist" || lo == "envelopelist") {
                if (opts_.include_envelopes) read_envelope_list();
                else skip_block_body();
            } else {
                skip_block_body();
            }
        }
        return true;
    }

    bool read_mesh(RawXsiMesh& rm, const Node& owner, std::string& err) {
        int nverts = 0;
        if (!read_int(nverts) || nverts < 0 || nverts > 5000000) {
            err = "Invalid Mesh vertex count in '" + rm.name + "'";
            return false;
        }
        rm.positions.resize(nverts);
        for (int i = 0; i < nverts; ++i) {
            if (!read_float(rm.positions[i].x) || !read_float(rm.positions[i].y) || !read_float(rm.positions[i].z)) {
                err = "Truncated vertex data in Mesh '" + rm.name + "'";
                return false;
            }
        }
        int nfaces = 0;
        if (!read_int(nfaces) || nfaces < 0 || nfaces > 5000000) {
            err = "Invalid Mesh face count in '" + rm.name + "'";
            return false;
        }
        rm.faces.resize(nfaces);
        for (int f = 0; f < nfaces; ++f) {
            int fv = 0;
            if (!read_int(fv) || fv < 0 || fv > 4096) {
                err = "Invalid face vertex count in Mesh '" + rm.name + "'";
                return false;
            }
            rm.faces[f].resize(fv);
            for (int k = 0; k < fv; ++k) {
                if (!read_int(rm.faces[f][k])) {
                    err = "Truncated face index in Mesh '" + rm.name + "'";
                    return false;
                }
            }
        }

        while (true) {
            auto pk = lex_.peek();
            if (pk.kind == XsiToken::Kind::Eof) break;
            if (pk.kind == XsiToken::Kind::RBrace) { lex_.next(); break; }
            std::string btype, bname;
            if (!read_block_header(btype, bname, err)) return false;
            std::string lo = to_lower(btype);
            if (lo == "meshmateriallist" || lo == "si_meshmateriallist") {
                read_material_list(rm, owner);
            } else if (lo == "si_meshnormals" || lo == "meshnormals") {
                read_indexed_vec3_attr(rm.normals, rm.normal_faces, nfaces);
            } else if (lo == "si_meshtexturecoords" || lo == "meshtexturecoords") {
                read_indexed_vec2_attr(rm.uvs, rm.uv_faces, nfaces);
            } else if (lo == "si_meshvertexcolors" || lo == "meshvertexcolors") {
                if (opts_.include_vertex_colors) read_indexed_vec4_attr(rm.vcols, rm.vcol_faces, nfaces);
                else skip_block_body();
            } else {
                skip_block_body();
            }
        }
        return true;
    }

    void read_indexed_vec3_attr(std::vector<Vec3>& vals, std::vector<std::vector<int>>& face_idx, int expected_faces) {
        int n = 0;
        if (!read_int(n) || n < 0 || n > 10000000) { skip_block_body(); return; }
        vals.resize(n);
        for (int i = 0; i < n; ++i) {
            if (!read_float(vals[i].x) || !read_float(vals[i].y) || !read_float(vals[i].z)) { skip_block_body(); return; }
        }
        int nf = 0;
        if (lex_.peek().kind == XsiToken::Kind::Word && read_int(nf) && nf >= 0 && nf <= 10000000) {
            face_idx.resize(std::max(nf, expected_faces));
            for (int f = 0; f < nf; ++f) {
                int target_face = 0, cnt = 0;
                if (!read_int(target_face) || !read_int(cnt) || cnt < 0 || cnt > 4096) break;
                std::vector<int> indices(cnt);
                for (int k = 0; k < cnt; ++k) read_int(indices[k]);
                if (target_face >= 0 && target_face < static_cast<int>(face_idx.size()))
                    face_idx[target_face] = std::move(indices);
            }
        }
        skip_block_body();
    }

    void read_indexed_vec2_attr(std::vector<Vec2>& vals, std::vector<std::vector<int>>& face_idx, int expected_faces) {
        int n = 0;
        if (!read_int(n) || n < 0 || n > 10000000) { skip_block_body(); return; }
        vals.resize(n);
        for (int i = 0; i < n; ++i) {
            if (!read_float(vals[i].x) || !read_float(vals[i].y)) { skip_block_body(); return; }
        }
        int nf = 0;
        if (lex_.peek().kind == XsiToken::Kind::Word && read_int(nf) && nf >= 0 && nf <= 10000000) {
            face_idx.resize(std::max(nf, expected_faces));
            for (int f = 0; f < nf; ++f) {
                int target_face = 0, cnt = 0;
                if (!read_int(target_face) || !read_int(cnt) || cnt < 0 || cnt > 4096) break;
                std::vector<int> indices(cnt);
                for (int k = 0; k < cnt; ++k) read_int(indices[k]);
                if (target_face >= 0 && target_face < static_cast<int>(face_idx.size()))
                    face_idx[target_face] = std::move(indices);
            }
        }
        skip_block_body();
    }

    void read_indexed_vec4_attr(std::vector<Vec4>& vals, std::vector<std::vector<int>>& face_idx, int expected_faces) {
        int n = 0;
        if (!read_int(n) || n < 0 || n > 10000000) { skip_block_body(); return; }
        vals.resize(n);
        for (int i = 0; i < n; ++i) {
            if (!read_float(vals[i].x) || !read_float(vals[i].y) || !read_float(vals[i].z) || !read_float(vals[i].w)) {
                skip_block_body(); return;
            }
        }
        int nf = 0;
        if (lex_.peek().kind == XsiToken::Kind::Word && read_int(nf) && nf >= 0 && nf <= 10000000) {
            face_idx.resize(std::max(nf, expected_faces));
            for (int f = 0; f < nf; ++f) {
                int target_face = 0, cnt = 0;
                if (!read_int(target_face) || !read_int(cnt) || cnt < 0 || cnt > 4096) break;
                std::vector<int> indices(cnt);
                for (int k = 0; k < cnt; ++k) read_int(indices[k]);
                if (target_face >= 0 && target_face < static_cast<int>(face_idx.size()))
                    face_idx[target_face] = std::move(indices);
            }
        }
        skip_block_body();
    }

    void read_material_list(RawXsiMesh& rm, const Node& owner) {
        int mat_count = 0, face_count = 0;
        if (!read_int(mat_count) || !read_int(face_count) || mat_count < 0 || face_count < 0) {
            skip_block_body();
            return;
        }
        rm.face_mat_indices.resize(face_count, 0);
        for (int i = 0; i < face_count; ++i) read_int(rm.face_mat_indices[i]);

        int mat_idx = 0;
        while (true) {
            auto pk = lex_.peek();
            if (pk.kind == XsiToken::Kind::Eof) break;
            if (pk.kind == XsiToken::Kind::RBrace) { lex_.next(); break; }
            std::string btype, bname, dummy_err;
            if (!read_block_header(btype, bname, dummy_err)) { skip_block_body(); break; }
            std::string lo = to_lower(btype);
            if (lo == "si_material" || lo == "material") {
                Material m = read_material(bname.empty() ? (owner.name + "_mat" + std::to_string(mat_idx)) : bname, owner);
                int scene_m_idx = add_or_reuse_material(m);
                rm.local_to_scene_mat.push_back(scene_m_idx);
                ++mat_idx;
            } else {
                skip_block_body();
            }
        }
        while (static_cast<int>(rm.local_to_scene_mat.size()) < mat_count) {
            Material def;
            def.name = owner.name + "_default_" + std::to_string(rm.local_to_scene_mat.size());
            rm.local_to_scene_mat.push_back(add_or_reuse_material(def));
        }
    }

    Material read_material(const std::string& name, const Node& owner) {
        Material m;
        m.name = name;
        m.double_sided = (owner.bz_flags.find('2') != std::string::npos);
        m.emissive_flag = (owner.bz_flags.find('e') != std::string::npos || owner.bz_flags.find('g') != std::string::npos);
        m.emissive_strength = (owner.bz_flags.find('g') != std::string::npos) ? 7.0f : 1.0f;
        m.no_texture_flag = (owner.bz_flags.find('F') != std::string::npos);

        // diffuse RGBA, hardness, specular RGB, emissive RGB, shading_type, ambient RGB
        bool ok = read_float(m.diffuse.x) && read_float(m.diffuse.y) && read_float(m.diffuse.z) && read_float(m.diffuse.w)
               && read_float(m.specular_power)
               && read_float(m.specular.x) && read_float(m.specular.y) && read_float(m.specular.z)
               && read_float(m.emissive.x) && read_float(m.emissive.y) && read_float(m.emissive.z)
               && read_int(m.shading_type)
               && read_float(m.ambient.x) && read_float(m.ambient.y) && read_float(m.ambient.z);
        if (!ok) {
            scene_.add_diag(DiagSeverity::Warning, "XSI_MATERIAL_FALLBACK",
                "Incomplete SI_Material '" + name + "', using defaults for remaining fields");
        }
        if (m.diffuse.w < 0.999f) m.has_alpha = true;

        while (true) {
            auto pk = lex_.peek();
            if (pk.kind == XsiToken::Kind::Eof) break;
            if (pk.kind == XsiToken::Kind::RBrace) { lex_.next(); break; }
            std::string btype, bname, dummy_err;
            if (!read_block_header(btype, bname, dummy_err)) { skip_block_body(); break; }
            std::string lo = to_lower(btype);
            if (lo == "si_texture2d" || lo == "texturefilename" || lo == "si_texture") {
                auto tex_tok = lex_.next();
                if (tex_tok.kind == XsiToken::Kind::String || tex_tok.kind == XsiToken::Kind::Word) {
                    if (!m.no_texture_flag) {
                        m.diffuse_tex.original_ref = tex_tok.text;
                        m.diffuse_tex.is_srgb = true;
                    }
                }
                skip_block_body();
            } else {
                skip_block_body();
            }
        }
        return m;
    }

    int add_or_reuse_material(const Material& m) {
        for (size_t i = 0; i < scene_.materials.size(); ++i) {
            const auto& e = scene_.materials[i];
            if (e.diffuse_tex.original_ref == m.diffuse_tex.original_ref &&
                e.double_sided == m.double_sided &&
                e.emissive_flag == m.emissive_flag &&
                e.no_texture_flag == m.no_texture_flag &&
                std::fabs(e.diffuse.x - m.diffuse.x) < 1e-4f &&
                std::fabs(e.diffuse.y - m.diffuse.y) < 1e-4f &&
                std::fabs(e.diffuse.z - m.diffuse.z) < 1e-4f &&
                std::fabs(e.diffuse.w - m.diffuse.w) < 1e-4f &&
                std::fabs(e.specular.x - m.specular.x) < 1e-4f &&
                std::fabs(e.specular_power - m.specular_power) < 1e-2f) {
                return static_cast<int>(i);
            }
        }
        int idx = static_cast<int>(scene_.materials.size());
        scene_.materials.push_back(m);
        return idx;
    }

    void read_animation_set() {
        while (true) {
            auto pk = lex_.peek();
            if (pk.kind == XsiToken::Kind::Eof) break;
            if (pk.kind == XsiToken::Kind::RBrace) { lex_.next(); break; }
            std::string btype, bname, dummy_err;
            if (!read_block_header(btype, bname, dummy_err)) { skip_block_body(); break; }
            if (to_lower(btype) == "animation" || to_lower(btype) == "si_animation") {
                read_animation(bname);
            } else {
                skip_block_body();
            }
        }
    }

    void read_animation(const std::string& anim_name) {
        RawAnim ra;
        ra.target_frame = clean_xsi_name(anim_name);
        while (true) {
            auto pk = lex_.peek();
            if (pk.kind == XsiToken::Kind::Eof) break;
            if (pk.kind == XsiToken::Kind::RBrace) { lex_.next(); break; }
            if (pk.kind == XsiToken::Kind::LBrace) {
                // Target frame reference: {frm-jnt6_1}
                lex_.next();
                auto ref = lex_.next();
                if (ref.kind == XsiToken::Kind::Word || ref.kind == XsiToken::Kind::String)
                    ra.target_frame = clean_xsi_name(ref.text);
                if (lex_.peek().kind == XsiToken::Kind::RBrace) lex_.next();
                continue;
            }
            std::string btype, bname, dummy_err;
            if (!read_block_header(btype, bname, dummy_err)) { skip_block_body(); break; }
            std::string lo = to_lower(btype);
            if (lo == "si_animationkey" || lo == "animationkey") {
                RawAnimKey ak;
                int nkeys = 0;
                if (read_int(ak.key_type) && read_int(nkeys) && nkeys >= 0 && nkeys < 1000000) {
                    for (int k = 0; k < nkeys; ++k) {
                        float frame = 0.0f;
                        int dim = 0;
                        if (!read_float(frame) || !read_int(dim) || dim <= 0 || dim > 16) break;
                        std::vector<float> vals(dim, 0.0f);
                        for (int d = 0; d < dim; ++d) read_float(vals[d]);
                        ak.keys.emplace_back(frame, std::move(vals));
                    }
                    ra.keys.push_back(std::move(ak));
                }
                skip_block_body();
            } else if (lo == "si_animationparamkey") {
                if (!saw_anim_param_key_) {
                    saw_anim_param_key_ = true;
                    scene_.add_diag(DiagSeverity::Warning, "XSI_UNSUPPORTED_ANIM_PARAM_KEY",
                        "XSI contains SI_AnimationParamKey (parameter animation not representable as standard transform tracks; skipped explicitly)");
                }
                skip_block_body();
            } else {
                skip_block_body();
            }
        }
        if (!ra.keys.empty()) raw_anims_.push_back(std::move(ra));
    }

    void read_envelope_list() {
        int declared_count = 0;
        if (lex_.peek().kind == XsiToken::Kind::Word) read_int(declared_count);
        while (true) {
            auto pk = lex_.peek();
            if (pk.kind == XsiToken::Kind::Eof) break;
            if (pk.kind == XsiToken::Kind::RBrace) { lex_.next(); break; }
            std::string btype, bname, dummy_err;
            if (!read_block_header(btype, bname, dummy_err)) { skip_block_body(); break; }
            if (to_lower(btype) == "si_envelope" || to_lower(btype) == "envelope") {
                RawEnvelope env;
                auto tmesh = lex_.next();
                auto tbone = lex_.next();
                env.mesh_frame = clean_xsi_name(tmesh.text);
                env.bone_frame = clean_xsi_name(tbone.text);
                int nw = 0;
                if (read_int(nw) && nw >= 0 && nw < 1000000) {
                    env.weights.reserve(nw);
                    for (int i = 0; i < nw; ++i) {
                        int vi = 0; float w = 0.0f;
                        if (!read_int(vi) || !read_float(w)) break;
                        env.weights.emplace_back(vi, w);
                    }
                }
                raw_envelopes_.push_back(std::move(env));
                skip_block_body();
            } else {
                skip_block_body();
            }
        }
    }

    void finalize_meshes() {
        struct CornerKey {
            int p, n, t, c;
            bool operator==(const CornerKey& o) const { return p == o.p && n == o.n && t == o.t && c == o.c; }
        };
        struct CornerHash {
            size_t operator()(const CornerKey& k) const {
                size_t h = std::hash<int>{}(k.p);
                h ^= std::hash<int>{}(k.n) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<int>{}(k.t) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<int>{}(k.c) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            }
        };

        for (auto& [node_idx, rm] : raw_meshes_) {
            if (rm.positions.empty() || rm.faces.empty()) {
                scene_.add_diag(DiagSeverity::Info, "XSI_EMPTY_MESH",
                    "Mesh '" + rm.name + "' on node '" + scene_.nodes[node_idx].name + "' has no geometry");
                continue;
            }
            Mesh mesh;
            mesh.name = rm.name;
            mesh.source_polygon_count = rm.faces.size();
            mesh.has_normals = !rm.normals.empty();
            mesh.has_uvs = !rm.uvs.empty();
            mesh.has_vertex_colors = !rm.vcols.empty();

            std::unordered_map<CornerKey, uint32_t, CornerHash> dedup;
            size_t bad_indices = 0;
            int npos = static_cast<int>(rm.positions.size());
            int nnorm = static_cast<int>(rm.normals.size());
            int nuv = static_cast<int>(rm.uvs.size());
            int nvcol = static_cast<int>(rm.vcols.size());

            for (size_t fi = 0; fi < rm.faces.size(); ++fi) {
                const auto& poly = rm.faces[fi];
                if (poly.size() < 3) continue;
                mesh.max_polygon_arity = std::max(mesh.max_polygon_arity, poly.size());
                bool poly_ok = true;
                for (int vi : poly) {
                    if (vi < 0 || vi >= npos) { poly_ok = false; ++bad_indices; break; }
                }
                if (!poly_ok) continue;

                int mat_scene_idx = -1;
                if (fi < rm.face_mat_indices.size()) {
                    int lm = rm.face_mat_indices[fi];
                    if (lm >= 0 && lm < static_cast<int>(rm.local_to_scene_mat.size()))
                        mat_scene_idx = rm.local_to_scene_mat[lm];
                } else if (!rm.local_to_scene_mat.empty()) {
                    mat_scene_idx = rm.local_to_scene_mat[0];
                }

                auto tris = triangulate_polygon(rm.positions, poly);
                for (const auto& tri : tris) {
                    for (int k = 0; k < 3; ++k) {
                        int corner = tri[k];
                        int pi = poly[corner];
                        int ni = -1;
                        if (mesh.has_normals) {
                            if (fi < rm.normal_faces.size() && corner < static_cast<int>(rm.normal_faces[fi].size())) {
                                int cand = rm.normal_faces[fi][corner];
                                if (cand >= 0 && cand < nnorm) ni = cand;
                            } else if (pi < nnorm) {
                                ni = pi;
                            }
                        }
                        int ti = -1;
                        if (mesh.has_uvs) {
                            if (fi < rm.uv_faces.size() && corner < static_cast<int>(rm.uv_faces[fi].size())) {
                                int cand = rm.uv_faces[fi][corner];
                                if (cand >= 0 && cand < nuv) ti = cand;
                            } else if (pi < nuv) {
                                ti = pi;
                            }
                        }
                        int ci = -1;
                        if (mesh.has_vertex_colors) {
                            if (fi < rm.vcol_faces.size() && corner < static_cast<int>(rm.vcol_faces[fi].size())) {
                                int cand = rm.vcol_faces[fi][corner];
                                if (cand >= 0 && cand < nvcol) ci = cand;
                            } else if (pi < nvcol) {
                                ci = pi;
                            }
                        }
                        CornerKey ck{pi, ni, ti, ci};
                        auto it = dedup.find(ck);
                        if (it == dedup.end()) {
                            MeshVertex mv;
                            mv.pos = rm.positions[pi];
                            if (ni >= 0) mv.normal = rm.normals[ni].normalized();
                            if (ti >= 0) mv.uv = rm.uvs[ti];
                            if (ci >= 0) mv.color = rm.vcols[ci];
                            mv.source_vertex_index = pi;
                            uint32_t new_idx = static_cast<uint32_t>(mesh.vertices.size());
                            mesh.vertices.push_back(mv);
                            it = dedup.emplace(ck, new_idx).first;
                        }
                        mesh.indices.push_back(it->second);
                    }
                    mesh.triangle_materials.push_back(mat_scene_idx);
                }
            }

            // Compute fallback smooth normals if mesh had no normals
            if (!mesh.has_normals && !mesh.indices.empty()) {
                for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                    auto& a = mesh.vertices[mesh.indices[i]];
                    auto& b = mesh.vertices[mesh.indices[i+1]];
                    auto& c = mesh.vertices[mesh.indices[i+2]];
                    Vec3 fn = (b.pos - a.pos).cross(c.pos - a.pos);
                    a.normal = a.normal + fn;
                    b.normal = b.normal + fn;
                    c.normal = c.normal + fn;
                }
                for (auto& v : mesh.vertices) v.normal = v.normal.normalized();
            }

            if (bad_indices > 0) {
                scene_.add_diag(DiagSeverity::Warning, "XSI_INVALID_FACE_INDICES",
                    "Mesh '" + rm.name + "' had " + std::to_string(bad_indices) + " out-of-range face vertex indices skipped");
            }
            int mesh_idx = static_cast<int>(scene_.meshes.size());
            scene_.meshes.push_back(std::move(mesh));
            scene_.nodes[node_idx].mesh_index = mesh_idx;
        }
    }

    void finalize_envelopes() {
        if (raw_envelopes_.empty()) return;
        // Map (mesh_node_index, source_vertex_index) -> list of MeshVertex indices
        std::unordered_map<int, std::unordered_map<int, std::vector<uint32_t>>> mesh_src_to_verts;
        for (size_t ni = 0; ni < scene_.nodes.size(); ++ni) {
            int mi = scene_.nodes[ni].mesh_index;
            if (mi < 0) continue;
            auto& tbl = mesh_src_to_verts[static_cast<int>(ni)];
            const auto& m = scene_.meshes[mi];
            for (uint32_t vi = 0; vi < m.vertices.size(); ++vi)
                tbl[m.vertices[vi].source_vertex_index].push_back(vi);
        }

        for (const auto& env : raw_envelopes_) {
            int mnode = scene_.find_node(env.mesh_frame);
            if (mnode < 0) mnode = scene_.find_node_ci(env.mesh_frame);
            int bnode = scene_.find_node(env.bone_frame);
            if (bnode < 0) bnode = scene_.find_node_ci(env.bone_frame);
            if (mnode < 0 || bnode < 0) {
                scene_.add_diag(DiagSeverity::Warning, "XSI_ENVELOPE_MISSING_NODE",
                    "Envelope references missing node (mesh='" + env.mesh_frame + "', bone='" + env.bone_frame + "')");
                continue;
            }
            scene_.nodes[bnode].is_bone = true;
            if (env.weights.empty()) continue;
            int mi = scene_.nodes[mnode].mesh_index;
            if (mi < 0) continue;
            auto& mesh = scene_.meshes[mi];
            if (!mesh.is_skinned) {
                mesh.is_skinned = true;
                mesh.skin_weights.resize(mesh.vertices.size());
            }
            const auto& src_map = mesh_src_to_verts[mnode];
            for (const auto& [src_vi, pct] : env.weights) {
                if (pct <= 0.0f) continue;
                auto it = src_map.find(src_vi);
                if (it == src_map.end()) continue;
                float w = pct / 100.0f;
                for (uint32_t dst_vi : it->second) {
                    mesh.skin_weights[dst_vi].push_back({bnode, w});
                }
            }
        }
    }

    static Quat euler_xyz_to_col_quat(float rx, float ry, float rz) {
        // XSI row-vector Euler XYZ -> column-vector quaternion = conjugate of standard XYZ
        float cx = std::cos(rx * 0.5f), sx = std::sin(rx * 0.5f);
        float cy = std::cos(ry * 0.5f), sy = std::sin(ry * 0.5f);
        float cz = std::cos(rz * 0.5f), sz = std::sin(rz * 0.5f);
        Quat q{
            cx * cy * cz + sx * sy * sz,
            sx * cy * cz - cx * sy * sz,
            cx * sy * cz + sx * cy * sz,
            cx * cy * sz - sx * sy * cz
        };
        return q.conjugate().normalized();
    }

    void finalize_animations() {
        if (raw_anims_.empty()) return;
        AnimationClip clip;
        clip.name = scene_.source_name.empty() ? "XSI_Animation" : scene_.source_name;
        clip.fps = 20.0f;
        float min_f = 1e30f, max_f = -1e30f;

        for (const auto& ra : raw_anims_) {
            int nidx = scene_.find_node(ra.target_frame);
            if (nidx < 0) nidx = scene_.find_node_ci(ra.target_frame);
            if (nidx < 0) {
                if (scene_.nodes.empty() || scene_.is_animation_only) {
                    scene_.is_animation_only = true;
                    Node placeholder;
                    placeholder.name = ra.target_frame;
                    placeholder.raw_name = ra.target_frame;
                    placeholder.is_bone = true;
                    nidx = static_cast<int>(scene_.nodes.size());
                    scene_.nodes.push_back(std::move(placeholder));
                } else {
                    scene_.add_diag(DiagSeverity::Warning, "XSI_ANIM_MISSING_NODE",
                        "Animation references unknown frame '" + ra.target_frame + "'");
                    continue;
                }
            }
            AnimTrack tr;
            tr.node_index = nidx;
            tr.node_name = scene_.nodes[nidx].name;
            for (const auto& ak : ra.keys) {
                for (const auto& [fr, vals] : ak.keys) {
                    min_f = std::min(min_f, fr);
                    max_f = std::max(max_f, fr);
                    if (ak.key_type == 0 && vals.size() >= 4) {
                        // XSI WXYZ row-vector quat -> standard column-vector quat = conjugate(w,x,y,z)
                        Quat q{vals[0], -vals[1], -vals[2], -vals[3]};
                        tr.rotations.push_back({fr, q.normalized()});
                    } else if (ak.key_type == 1 && vals.size() >= 3) {
                        tr.scales.push_back({fr, {vals[0], vals[1], vals[2]}});
                    } else if (ak.key_type == 2 && vals.size() >= 3) {
                        tr.translations.push_back({fr, {vals[0], vals[1], vals[2]}});
                    } else if (ak.key_type == 3 && vals.size() >= 3) {
                        constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
                        float factor = (angle_units_ == 1) ? 1.0f : kDeg2Rad;
                        tr.rotations.push_back({fr, euler_xyz_to_col_quat(vals[0]*factor, vals[1]*factor, vals[2]*factor)});
                    } else {
                        scene_.add_diag(DiagSeverity::Warning, "XSI_UNKNOWN_ANIM_KEYTYPE",
                            "Unsupported SI_AnimationKey type " + std::to_string(ak.key_type) + " on '" + tr.node_name + "'");
                    }
                }
            }
            if (!tr.translations.empty() || !tr.rotations.empty() || !tr.scales.empty())
                clip.tracks.push_back(std::move(tr));
        }

        if (!clip.tracks.empty()) {
            clip.start_frame = (min_f <= max_f) ? min_f : 0.0f;
            clip.end_frame = (min_f <= max_f) ? max_f : 0.0f;
            scene_.animations.push_back(std::move(clip));
        }
    }
};

} // namespace

bool parse_xsi_bytes(const std::vector<uint8_t>& bytes, const std::filesystem::path& source_path,
                     Scene& out_scene, std::string& out_error, const XsiParseOptions& opts) {
    out_scene = Scene{};
    out_scene.source_path = utf8_from_path(source_path);
    out_scene.source_name = utf8_from_path(source_path.stem());
    out_scene.uv_origin_bottom_left = true;
    if (!bytes.empty()) out_scene.source_sha256 = sha256_hex(bytes.data(), bytes.size());

    if (bytes.size() < 12) {
        out_error = "XSI file too short";
        return false;
    }
    XsiLexer lex(bytes.data(), bytes.size());
    XsiParserImpl parser(lex, out_scene, opts);
    return parser.parse(out_error);
}

bool parse_xsi_file(const std::filesystem::path& path, Scene& out_scene, std::string& out_error,
                    const XsiParseOptions& opts) {
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(path, bytes, out_error)) return false;
    return parse_xsi_bytes(bytes, path, out_scene, out_error, opts);
}

} // namespace bz
