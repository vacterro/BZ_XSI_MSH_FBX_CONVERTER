// T-9 (SAIHANDOFF-11): six independently reproduced defects, each printing a
// [T9] marker on success; tools/regression_check_T9.py greps the markers.
// GLB reconstruction here uses the glTF 2.0 column-vector TRS contract
// implemented independently of the exporter's row-vector math.
#include "bzconverter/core/types.hpp"
#include "bzconverter/core/json.hpp"
#include "bzconverter/scene/scene.hpp"
#include "bzconverter/exporters/glb_exporter.hpp"
#include "bzconverter/validation/validator.hpp"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <zlib.h>

namespace bztests_t9 {

namespace fs = std::filesystem;
static int t9_run = 0, t9_pass = 0;

#define T9_CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  T9 FAILED: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            return false; \
        } \
    } while (0)

#define RUN_T9(fn) \
    do { \
        t9_run++; \
        std::cout << "[RUN]  " << #fn << "..." << std::flush; \
        if (fn()) { t9_pass++; std::cout << " PASSED\n"; } \
    } while (0)

// ------------------------------------------------------------------ GLB IO
struct GlbDoc {
    bz::Json json;
    std::vector<uint8_t> bin;
};

uint32_t rd32(const std::vector<uint8_t>& b, size_t o) {
    return uint32_t(b[o]) | (uint32_t(b[o+1]) << 8) | (uint32_t(b[o+2]) << 16) | (uint32_t(b[o+3]) << 24);
}

bool parse_glb(const std::vector<uint8_t>& b, GlbDoc& g, std::string& err) {
    if (b.size() < 20 || rd32(b, 0) != 0x46546C67u) { err = "not a GLB"; return false; }
    size_t off = 12;
    while (off + 8 <= b.size()) {
        uint32_t len = rd32(b, off), type = rd32(b, off + 4);
        if (off + 8 + len > b.size()) { err = "chunk overruns"; return false; }
        if (type == 0x4E4F534Au) {
            std::string js(reinterpret_cast<const char*>(b.data()) + off + 8, len);
            if (!bz::Json::parse(js, g.json, err)) return false;
        } else if (type == 0x004E4942u) {
            g.bin.assign(b.begin() + off + 8, b.begin() + off + 8 + len);
        }
        off += 8 + len;
    }
    return true;
}

std::vector<float> read_vec(const GlbDoc& g, int acc_idx) {
    std::vector<float> out;
    const auto* accessors = g.json.get("accessors");
    if (!accessors) return out;
    const auto& acc = accessors->as_arr()[acc_idx];
    std::string type = acc.get("type")->as_str();
    int comps = type == "VEC3" ? 3 : (type == "VEC2" ? 2 : (type == "VEC4" ? 4 : 1));
    size_t count = size_t(acc.get("count")->as_num());
    const auto& bv = g.json.get("bufferViews")->as_arr()[acc.get("bufferView")->as_int()];
    size_t base = size_t(bv.get("byteOffset")->as_num());
    out.resize(count * comps);
    for (size_t i = 0; i < out.size(); ++i) std::memcpy(&out[i], g.bin.data() + base + i * 4, 4);
    return out;
}

// ------------------------------------------- independent glTF evaluation
// Column-vector node world matrix (16 floats, m[row*4+col], column-vector
// math: p_world[col] = sum p[i] * m[i*4+col]).
void node_world(const bz::Json& doc, int node, float m[16]) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    const auto& n = doc.as_obj().at("nodes").as_arr()[node];
    float t[3] = {0,0,0}, s[3] = {1,1,1}, q[4] = {0,0,0,1};
    if (const auto* v = n.get("translation")) for (int i=0;i<3;++i) t[i] = v->as_arr()[i].as_num();
    if (const auto* v = n.get("scale")) for (int i=0;i<3;++i) s[i] = v->as_arr()[i].as_num();
    if (const auto* v = n.get("rotation")) for (int i=0;i<4;++i) q[i] = v->as_arr()[i].as_num();
    float r[9] = {
        1-2*(q[1]*q[1]+q[2]*q[2]), 2*(q[0]*q[1]-q[3]*q[2]),   2*(q[0]*q[2]+q[3]*q[1]),
        2*(q[0]*q[1]+q[3]*q[2]),   1-2*(q[0]*q[0]+q[2]*q[2]), 2*(q[1]*q[2]-q[3]*q[0]),
        2*(q[0]*q[2]-q[3]*q[1]),   2*(q[1]*q[2]+q[3]*q[0]),   1-2*(q[0]*q[0]+q[1]*q[1]),
    };
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            m[row*4+col] = r[row*3+col] * s[col];
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1.0f;
    int parent = -1;
    const auto& nodes = doc.as_obj().at("nodes").as_arr();
    for (size_t pi = 0; pi < nodes.size() && parent < 0; ++pi) {
        const auto* ch = nodes[pi].get("children");
        if (!ch) continue;
        for (const auto& c : ch->as_arr()) if (c.as_int() == node) parent = static_cast<int>(pi);
    }
    if (parent >= 0 && parent != node) {
        float pw[16];
        node_world(doc, parent, pw);
        float res[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                for (int k = 0; k < 4; ++k)
                    res[row*4+col] += m[k*4+col] * pw[row*4+k];
        for (int i = 0; i < 16; ++i) m[i] = res[i];
    }
}

void apply_point(const float m[16], const float p[3], float out[3]) {
    for (int j = 0; j < 3; ++j)
        out[j] = p[0]*m[0*4+j] + p[1]*m[1*4+j] + p[2]*m[2*4+j] + m[3*4+j];
}

// world normal = inverse-transpose(upper3x3) * local normal, per glTF spec.
// m is column-vector world matrix with m[row*4+col]; A = upper 3x3.
// A^(-T) applied as a column-vector transform:
// (A^(-T) n)_row = sum_k invT[row][k] * n[k], where invT = transpose(invA).
void apply_normal(const float m[16], const float n_in[3], float n_out[3]) {
    float a[9] = {m[0],m[1],m[2], m[4],m[5],m[6], m[8],m[9],m[10]}; // a[row*3+col]
    float det = a[0]*(a[4]*a[8]-a[5]*a[7]) - a[1]*(a[3]*a[8]-a[5]*a[6]) + a[2]*(a[3]*a[7]-a[4]*a[6]);
    if (std::fabs(det) < 1e-12f) { n_out[0]=n_in[0]; n_out[1]=n_in[1]; n_out[2]=n_in[2]; return; }
    // invA[row][col]
    float inv[9] = {
        (a[4]*a[8]-a[5]*a[7])/det, (a[2]*a[7]-a[1]*a[8])/det, (a[1]*a[5]-a[2]*a[4])/det,
        (a[5]*a[6]-a[3]*a[8])/det, (a[0]*a[8]-a[2]*a[6])/det, (a[2]*a[3]-a[0]*a[5])/det,
        (a[3]*a[7]-a[4]*a[6])/det, (a[1]*a[6]-a[0]*a[7])/det, (a[0]*a[4]-a[1]*a[3])/det,
    };
    // A^(-T)[row][col] = invA[col][row]
    for (int row = 0; row < 3; ++row)
        n_out[row] = n_in[0]*inv[0*3+row] + n_in[1]*inv[1*3+row] + n_in[2]*inv[2*3+row];
    float len = std::sqrt(n_out[0]*n_out[0] + n_out[1]*n_out[1] + n_out[2]*n_out[2]);
    if (len > 1e-12f) for (int i = 0; i < 3; ++i) n_out[i] /= len;
}

// ------------------------------------------------------------ fixtures
bool export_ok(bz::Scene& scene, const fs::path& out, std::vector<bz::Diagnostic>& diags,
               std::string& err, bool include_mats = false) {
    bz::ExportOptions opts;
    opts.include_materials = include_mats;
    opts.include_textures = include_mats;
    opts.overwrite = true;
    std::error_code ec; fs::remove(out, ec);
    return bz::export_scene_gltf(scene, out, true, opts, diags, err);
}

bool read_glb_file(const fs::path& p, GlbDoc& g, std::string& err) {
    std::vector<uint8_t> bytes;
    if (!bz::read_file_bytes(p, bytes, err)) return false;
    return parse_glb(bytes, g, err);
}

// Small skin fixture helper shared by several tests.
void make_skin_fixture(bz::Scene& scene) {
    scene.source_name = "t9_skin_fixture";
    bz::Node mesh_node; mesh_node.name = "mesh";
    bz::Node bone; bone.name = "bone"; bone.is_bone = true;
    scene.nodes.push_back(mesh_node);
    scene.nodes.push_back(bone);
    bz::Mesh m; m.name = "skinned"; m.is_skinned = true;
    m.vertices.resize(3);
    for (auto& v : m.vertices) v.pos = {0.0f, 0.0f, 0.0f};
    m.indices = {0, 1, 2};
    m.skin_weights = {{{1, 1.0f}}, {{1, 1.0f}}, {{1, 1.0f}}};
    scene.meshes.push_back(m);
    scene.nodes[0].mesh_index = 0;
    scene.root_nodes.push_back(0);
    scene.root_nodes.push_back(1);
}

// ------------------------------------------------------- 1. world normals
bool t9_world_space_normal_reconstruction() {
    // The acceptance contract from the handoff: for every vertex,
    // source_world_normal ~= imported_world_normal. The source normal IS a
    // world direction, so the reconstructed world normal must equal the
    // input direction for every transform in the matrix. Cases: identity,
    // translation, rot +-90 Z, rot 90 X, rot 90 Y, nonuniform scale, the
    // handoff witness (rot 90 Z + scale (2,1,1)), mirror, parent rot +
    // child scale, nested chain.
    struct Case {
        std::vector<bz::Mat4> locals; // node chain root..leaf (row-vector)
        float src[3];                 // source world normal (unit length)
    };
    std::vector<Case> cases;
    auto rotq = [](float x, float y, float z, float w) { return bz::Quat{w, x, y, z}; };
    auto mk = [&](std::vector<bz::Mat4> locals, float sx, float sy, float sz) {
        Case c; c.locals = std::move(locals);
        c.src[0]=sx; c.src[1]=sy; c.src[2]=sz;
        return c;
    };

    cases.push_back(mk({bz::Mat4::identity()}, 0.0f, 1.0f, 0.0f));
    {
        bz::Mat4 m = bz::Mat4::identity(); m.m[12] = 7.0f; m.m[13] = -3.0f;
        cases.push_back(mk({m}, 1.0f, 0.0f, 0.0f));
    }
    cases.push_back(mk({bz::Mat4::from_bz_trs({0,0,0}, rotq(0,0,0.70710678f,0.70710678f))},
                        0.0f, 1.0f, 0.0f));
    cases.push_back(mk({bz::Mat4::from_bz_trs({0,0,0}, rotq(0,0,-0.70710678f,0.70710678f))},
                        0.0f, 1.0f, 0.0f));
    cases.push_back(mk({bz::Mat4::from_bz_trs({0,0,0}, rotq(0.70710678f,0,0,0.70710678f))},
                        0.0f, 1.0f, 0.0f));
    cases.push_back(mk({bz::Mat4::from_bz_trs({0,0,0}, rotq(0,0.70710678f,0,0.70710678f))},
                        1.0f, 0.0f, 0.0f));
    {
        bz::Mat4 m = bz::Mat4::identity(); m.m[0] = 2.0f;
        cases.push_back(mk({m}, 1.0f, 0.0f, 0.0f));
    }
    // The handoff witness: world normal (1,0,0) must reconstruct as (1,0,0)
    cases.push_back(mk({bz::Mat4::from_bz_trs({0,0,0}, rotq(0,0,0.70710678f,0.70710678f), {2,1,1})},
                        1.0f, 0.0f, 0.0f));
    // Mirror (negative X): the same contract holds without sign games
    {
        bz::Mat4 m = bz::Mat4::identity(); m.m[0] = -1.0f;
        cases.push_back(mk({m}, 1.0f, 0.0f, 0.0f));
    }
    {
        bz::Mat4 root = bz::Mat4::from_bz_trs({0,0,0}, rotq(0,0,0.70710678f,0.70710678f));
        bz::Mat4 child = bz::Mat4::identity(); child.m[5] = 3.0f;
        cases.push_back(mk({root, child}, 0.0f, 1.0f, 0.0f));
    }
    {
        bz::Mat4 a = bz::Mat4::from_bz_trs({0,0,0}, rotq(0,0,0.70710678f,0.70710678f));
        bz::Mat4 b = bz::Mat4::identity(); b.m[0] = 2.0f; b.m[10] = 0.5f;
        bz::Mat4 c = bz::Mat4::identity(); c.m[12] = 4.0f; c.m[13] = 5.0f;
        cases.push_back(mk({a, b, c}, 1.0f, 0.0f, 0.0f));
    }

    fs::path out = fs::temp_directory_path() / "bzconv_t9_world_normal.glb";
    for (size_t ci = 0; ci < cases.size(); ++ci) {
        const auto& c = cases[ci];
        bz::Scene scene;
        scene.source_name = "t9_world_normal";
        for (size_t k = 0; k < c.locals.size(); ++k) {
            bz::Node n;
            n.name = "node" + std::to_string(k);
            n.local_matrix = c.locals[k];
            n.parent_index = k == 0 ? -1 : static_cast<int>(k - 1);
            scene.nodes.push_back(n);
        }
        // Row-vector world composition: leaf geometry is posed by
        // world = local_0 * local_1 * ... (root applied first).
        bz::Mat4 world = c.locals[0];
        for (size_t k = 1; k < c.locals.size(); ++k) world = world * c.locals[k];

        bz::Mesh m;
        m.name = "m";
        m.vertices_in_model_space = true;
        m.vertices.resize(1);
        m.vertices[0].pos = world.transform_point({0.0f, 0.0f, 0.0f});
        m.vertices[0].normal = {c.src[0], c.src[1], c.src[2]};
        m.indices = {0, 0, 0};
        scene.meshes.push_back(m);
        int leaf = static_cast<int>(c.locals.size() - 1);
        scene.nodes[leaf].mesh_index = 0;
        scene.root_nodes.push_back(0);

        std::vector<bz::Diagnostic> diags; std::string err;
        T9_CHECK(export_ok(scene, out, diags, err),
                 ("case " + std::to_string(ci) + " export: " + err).c_str());
        GlbDoc glb;
        T9_CHECK(read_glb_file(out, glb, err), ("case read: " + err).c_str());
        const auto* meshes = glb.json.get("meshes");
        T9_CHECK(meshes && !meshes->as_arr().empty(), "meshes present");
        const auto* prims = meshes->as_arr()[0].get("primitives");
        const auto* attrs = prims->as_arr()[0].get("attributes");
        auto norm = read_vec(glb, attrs->get("NORMAL")->as_int());
        T9_CHECK(norm.size() == 3, "one normal");
        float wm[16];
        node_world(glb.json, leaf, wm);
        float nl[3] = {norm[0], norm[1], norm[2]}, nw[3];
        apply_normal(wm, nl, nw);
        // Source normal direction must survive the full scene evaluation.
        float sl = std::sqrt(c.src[0]*c.src[0] + c.src[1]*c.src[1] + c.src[2]*c.src[2]);
        float dot = (nw[0]*c.src[0] + nw[1]*c.src[1] + nw[2]*c.src[2]) / sl;
        T9_CHECK(dot > 0.9999f,
                 ("case " + std::to_string(ci) + " world normal (" +
                  std::to_string(nw[0]) + "," + std::to_string(nw[1]) + "," + std::to_string(nw[2]) +
                  ") vs source (" + std::to_string(c.src[0]) + "," + std::to_string(c.src[1]) +
                  "," + std::to_string(c.src[2]) + ") dot=" + std::to_string(dot)).c_str());
    }
    std::error_code ec; fs::remove(out, ec);
    std::cout << "[T9] t9_world_space_normal_reconstruction PASSED\n";
    return true;
}

// ------------------------------------------------- 2. short skin weights
bool t9_missing_joint_attributes() {
    bz::Scene scene;
    make_skin_fixture(scene);
    // Break the contract: 3 vertices, only 2 weight lists.
    scene.meshes[0].skin_weights.pop_back();

    bz::ExportOptions opts;
    opts.include_materials = false; opts.include_textures = false; opts.overwrite = true;
    fs::path out = fs::temp_directory_path() / "bzconv_t9_short_skin.glb";
    std::error_code ec; fs::remove(out, ec);
    std::vector<bz::Diagnostic> diags; std::string err;
    T9_CHECK(bz::export_scene_gltf(scene, out, true, opts, diags, err), ("export: " + err).c_str());

    GlbDoc glb;
    T9_CHECK(read_glb_file(out, glb, err), ("read: " + err).c_str());
    bool skin_ref = false;
    for (const auto& n : glb.json.get("nodes")->as_arr())
        if (n.get("skin")) skin_ref = true;
    const auto* meshes = glb.json.get("meshes");
    const auto* prims = meshes->as_arr()[0].get("primitives");
    const auto* attrs = prims->as_arr()[0].get("attributes");
    bool joints = attrs->get("JOINTS_0") != nullptr;
    bool weights = attrs->get("WEIGHTS_0") != nullptr;
    T9_CHECK(!(skin_ref && !(joints && weights)),
             "skin reference without JOINTS/WEIGHTS must never be exported");

    bool dropped = false;
    for (const auto& d : diags) if (d.code == "GLB_SKIN_DATA_INCOMPLETE") dropped = true;
    T9_CHECK(dropped, "expected GLB_SKIN_DATA_INCOMPLETE diagnostic");

    bz::ExportResult r;
    r.status = bz::JobStatus::Success;
    r.result_class = bz::ResultClass::SuccessUnverified;
    r.diagnostics = diags;
    bz::adjudicate_result(r, opts, scene.compute_stats());
    T9_CHECK(r.result_class == bz::ResultClass::Partial,
             "incomplete skin must adjudicate to PARTIAL, not SUCCESS");

    // Also add a diagnostic policy so the whole pipeline, not just the
    // direct call, classifies it honestly.
    T9_CHECK(bz::diagnostic_forces_partial(
                 {bz::DiagSeverity::Error, "GLB_SKIN_DATA_INCOMPLETE", ""}),
             "GLB_SKIN_DATA_INCOMPLETE must force PARTIAL via policy");

    fs::remove(out, ec);
    std::cout << "[T9] t9_missing_joint_attributes PASSED\n";
    return true;
}

// --------------------------------------------------- 3. invalid JPEG SOF
const unsigned char kBadSofJpeg[] = {
    0xFF,0xD8,
    0xFF,0xDB,0x00,0x43,0x00,
    0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
    0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
    0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
    0xFF,0xC0,0x00,0x0F, 0x00, 0x00,0x04, 0x00,0x04, 0x03,
    0x01,0x22,0x00, 0x02,0x11,0x01, 0x03,0x11,0x01,
    0xFF,0xDA,0x00,0x08, 0x01, 0x01,0x00,0x00, 0x3F,0x00,
    0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
    0xFF,0xD9
};

bool t9_invalid_jpeg_rejected() {
    fs::path dir = fs::temp_directory_path() / "bzconv_t9_bad_jpeg";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir, ec);
    fs::path jp = dir / "bad_sof.jpg";
    std::string err;
    T9_CHECK(bz::write_file_atomic(jp, kBadSofJpeg, sizeof(kBadSofJpeg), err), "write fixture");

    bz::Scene scene;
    make_skin_fixture(scene);
    bz::Material mat; mat.name = "mat"; scene.materials.push_back(mat);
    scene.materials[0].diffuse_tex.resolved_path = bz::utf8_from_path(jp);

    fs::path out = dir / "model.glb";
    std::vector<bz::Diagnostic> diags;
    T9_CHECK(export_ok(scene, out, diags, err, true), "export with material");
    bool invalid = false;
    for (const auto& d : diags) if (d.code == "GLTF_TEXTURE_INVALID") invalid = true;
    T9_CHECK(invalid, "bad-precision JPEG must be GLTF_TEXTURE_INVALID");

    GlbDoc glb;
    T9_CHECK(read_glb_file(out, glb, err), "read glb");
    const auto* images = glb.json.get("images");
    T9_CHECK(!images || images->as_arr().empty(), "undecodable JPEG must not be embedded");

    bz::ExportOptions opts;
    opts.include_materials = true; opts.include_textures = true; opts.overwrite = true;
    bz::ExportResult r;
    r.status = bz::JobStatus::Success;
    r.result_class = bz::ResultClass::SuccessUnverified;
    r.diagnostics = diags;
    bz::adjudicate_result(r, opts, scene.compute_stats());
    T9_CHECK(r.result_class == bz::ResultClass::Partial, "invalid JPEG must be PARTIAL");

    fs::remove_all(dir, ec);
    std::cout << "[T9] t9_invalid_jpeg_rejected PASSED\n";
    return true;
}

// ----------------------------------------------- 4. valid indexed PNGs
bool build_indexed_png(int bits, int w, int h, std::vector<uint8_t>& out) {
    out = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};
    auto push_be32 = [&](std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(uint8_t(x>>24)); v.push_back(uint8_t(x>>16));
        v.push_back(uint8_t(x>>8)); v.push_back(uint8_t(x));
    };
    auto push_chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        std::vector<uint8_t> c;
        push_be32(c, uint32_t(data.size()));
        for (int i = 0; i < 4; ++i) c.push_back(uint8_t(type[i]));
        c.insert(c.end(), data.begin(), data.end());
        uint32_t crc = uint32_t(crc32(crc32(0L, Z_NULL, 0), c.data() + 4, data.size() + 4));
        push_be32(c, crc);
        out.insert(out.end(), c.begin(), c.end());
    };
    std::vector<uint8_t> ihdr(13);
    ihdr[3] = uint8_t(w); ihdr[7] = uint8_t(h);
    ihdr[8] = uint8_t(bits); ihdr[9] = 3; // indexed
    push_chunk("IHDR", ihdr);
    const uint8_t plte[12] = {0,0,0, 255,0,0, 0,255,0, 255,255,0};
    push_chunk("PLTE", std::vector<uint8_t>(plte, plte + 12));
    // scanlines: filter 0 + packed index bytes; every pixel uses palette
    // index 3 (in range of the 4-entry PLTE) for each supported bit depth.
    size_t row_bytes = size_t(w) * bits / 8 + ((size_t(w) * bits % 8) ? 1 : 0);
    std::vector<uint8_t> raw(size_t(h) * (row_bytes + 1));
    uint8_t pattern = 0;
    switch (bits) {
        case 1: pattern = 0xFF; break; // eight index-3 pixels per byte
        case 2: pattern = 0xFF; break; // four index-3 pixels per byte
        case 4: pattern = 0x33; break; // two index-3 pixels per byte
        case 8: pattern = 0x03; break; // one index-3 pixel per byte
    }
    for (int y = 0; y < h; ++y) {
        raw[size_t(y) * (row_bytes + 1)] = 0; // filter none
        for (size_t b = 0; b < row_bytes; ++b)
            raw[size_t(y) * (row_bytes + 1) + 1 + b] = pattern;
    }
    uLongf comp_len = compressBound(raw.size());
    std::vector<uint8_t> comp(comp_len);
    if (compress(comp.data(), &comp_len, raw.data(), uLong(raw.size())) != Z_OK) return false;
    comp.resize(comp_len);
    push_chunk("IDAT", comp);
    push_chunk("IEND", {});
    return true;
}

bool t9_valid_indexed_png_accepted() {
    fs::path dir = fs::temp_directory_path() / "bzconv_t9_indexed_png";
    std::error_code ec; fs::remove_all(dir, ec); fs::create_directories(dir, ec);
    for (int bits : {1, 2, 4, 8}) {
        std::vector<uint8_t> png;
        T9_CHECK(build_indexed_png(bits, 4, 4, png), ("build png bits=" + std::to_string(bits)).c_str());
        fs::path pp = dir / ("idx" + std::to_string(bits) + ".png");
        std::string err;
        T9_CHECK(bz::write_file_atomic(pp, png.data(), png.size(), err), "write png");

        bz::Scene scene;
        make_skin_fixture(scene);
        bz::Material mat; mat.name = "mat"; scene.materials.push_back(mat);
        scene.materials[0].diffuse_tex.resolved_path = bz::utf8_from_path(pp);
        fs::path out = dir / "model.glb";
        std::vector<bz::Diagnostic> diags;
        T9_CHECK(export_ok(scene, out, diags, err, true), ("export bits=" + std::to_string(bits)).c_str());
        bool invalid = false;
        for (const auto& d : diags) if (d.code == "GLTF_TEXTURE_INVALID") invalid = true;
        T9_CHECK(!invalid, ("valid indexed PNG bits=" + std::to_string(bits) + " rejected as corrupt").c_str());
        GlbDoc glb;
        T9_CHECK(read_glb_file(out, glb, err), "read glb");
        const auto* images = glb.json.get("images");
        T9_CHECK(images && !images->as_arr().empty(), ("indexed PNG bits=" + std::to_string(bits) + " not embedded").c_str());
    }
    fs::remove_all(dir, ec);
    std::cout << "[T9] t9_valid_indexed_png_accepted PASSED\n";
    return true;
}

// ---------------------------------------------------- 5. cycle not serialized
bool t9_cycle_not_serialized() {
    bz::Scene scene;
    scene.source_name = "t9_cycle";
    bz::Node a; a.name = "a"; a.parent_index = 1; a.children.push_back(1);
    bz::Node b; b.name = "b"; b.parent_index = 0; b.children.push_back(0);
    scene.nodes.push_back(a);
    scene.nodes.push_back(b);
    bz::Mesh m; m.name = "m";
    m.vertices.resize(1); m.vertices[0].pos = {0,0,0};
    m.indices = {0,0,0};
    scene.meshes.push_back(m);
    scene.nodes[0].mesh_index = 0;

    fs::path out = fs::temp_directory_path() / "bzconv_t9_cycle.glb";
    std::vector<bz::Diagnostic> diags; std::string err;
    T9_CHECK(export_ok(scene, out, diags, err), "export");
    bool cycle_diag = false;
    for (const auto& d : diags) if (d.code == "GLB_HIERARCHY_CYCLE") cycle_diag = true;
    T9_CHECK(cycle_diag, "expected GLB_HIERARCHY_CYCLE diagnostic");

    GlbDoc glb;
    T9_CHECK(read_glb_file(out, glb, err), "read glb");
    const auto* nodes_j = glb.json.get("nodes");
    size_t n = nodes_j->as_arr().size();
    std::vector<int> parent_of(n, -1);
    for (size_t pi = 0; pi < n; ++pi) {
        const auto* ch = nodes_j->as_arr()[pi].get("children");
        if (!ch) continue;
        for (const auto& c : ch->as_arr()) {
            int ci = c.as_int();
            T9_CHECK(ci >= 0 && ci < int(n), "child index in range");
            T9_CHECK(parent_of[size_t(ci)] == -1, "a node is listed as a child twice");
            parent_of[size_t(ci)] = int(pi);
        }
    }
    for (size_t ni = 0; ni < n; ++ni) {
        std::vector<char> seen(n, 0);
        int cur = int(ni);
        while (cur >= 0) {
            T9_CHECK(!seen[size_t(cur)], "serialized hierarchy contains a cycle");
            seen[size_t(cur)] = 1;
            cur = parent_of[size_t(cur)];
        }
    }
    std::error_code ec; fs::remove(out, ec);
    std::cout << "[T9] t9_cycle_not_serialized PASSED\n";
    return true;
}

// ---------------------------------------------------- 6. instance localization
bool t9_instance_localization() {
    bz::Scene scene;
    scene.source_name = "t9_instances";
    bz::Node a; a.name = "A"; a.local_matrix.m[12] = 5.0f;
    bz::Node b; b.name = "B"; b.local_matrix.m[12] = 9.0f;
    scene.nodes.push_back(a);
    scene.nodes.push_back(b);
    bz::Mesh m; m.name = "shared"; m.vertices_in_model_space = true;
    m.vertices.resize(1);
    m.vertices[0].pos = {5.0f, 0.0f, 0.0f};
    m.vertices[0].normal = {0.0f, 1.0f, 0.0f};
    m.indices = {0,0,0};
    scene.meshes.push_back(m);
    scene.nodes[0].mesh_index = 0;
    scene.nodes[1].mesh_index = 0;
    scene.root_nodes.push_back(0);
    scene.root_nodes.push_back(1);

    fs::path out = fs::temp_directory_path() / "bzconv_t9_instance.glb";
    std::vector<bz::Diagnostic> diags; std::string err;
    T9_CHECK(export_ok(scene, out, diags, err), "export");

    GlbDoc glb;
    T9_CHECK(read_glb_file(out, glb, err), "read glb");
    const auto* nodes_j = glb.json.get("nodes");
    // Contract: the shared model-space vertex (world X=5) must stay at its
    // authored position under BOTH instances — the pre-fix single buffer
    // localized to node A teleported instance B's copy to world X=9. Each
    // instance gets its own localized buffer; each node keeps its own TRS.
    std::vector<float> found_x;
    std::vector<int> instance_meshes;
    for (size_t ni = 0; ni < nodes_j->as_arr().size(); ++ni) {
        const auto* mesh_ref = nodes_j->as_arr()[ni].get("mesh");
        if (!mesh_ref) continue;
        int gmi = mesh_ref->as_int();
        instance_meshes.push_back(gmi);
        const auto& gm = glb.json.get("meshes")->as_arr()[size_t(gmi)];
        const auto* prims = gm.get("primitives");
        const auto* attrs = prims->as_arr()[0].get("attributes");
        auto pos = read_vec(glb, attrs->get("POSITION")->as_int());
        float wm[16];
        node_world(glb.json, int(ni), wm);
        float p[3] = {pos[0], pos[1], pos[2]}, pw[3];
        apply_point(wm, p, pw);
        found_x.push_back(pw[0]);
        T9_CHECK(std::fabs(pw[0] - 5.0f) < 1e-3f,
                 ("instance world X " + std::to_string(pw[0]) + " (expected 5, the authored model-space position)").c_str());
    }
    T9_CHECK(found_x.size() == 2, "both instances must exist");
    // Per-instance buffers, not one shared mesh reused twice.
    T9_CHECK(instance_meshes[0] != instance_meshes[1],
             "each instance must reference its own localized glTF mesh");
    std::error_code ec; fs::remove(out, ec);
    std::cout << "[T9] t9_instance_localization PASSED\n";
    return true;
}

int run_all() {
    std::cout << "---- T-9 (SAIHANDOFF-11) regression cases ----\n";
    RUN_T9(t9_world_space_normal_reconstruction);
    RUN_T9(t9_missing_joint_attributes);
    RUN_T9(t9_invalid_jpeg_rejected);
    RUN_T9(t9_valid_indexed_png_accepted);
    RUN_T9(t9_cycle_not_serialized);
    RUN_T9(t9_instance_localization);
    return t9_run == t9_pass ? 0 : 1;
}

} // namespace bztests_t9
