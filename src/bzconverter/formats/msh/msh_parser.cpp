#include "bzconverter/formats/msh/msh_parser.hpp"
#include "bzconverter/core/sha256.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace bz {

namespace {

constexpr uint32_t kMATERIAL = 0x9709513eu;
constexpr uint32_t kMATERIAL_END = 0x9709513fu;
constexpr uint32_t kTEXTURE = 0x7951fc0bu;
constexpr uint32_t kMRM_BLOCK = 0xf18f2bdeu;
constexpr uint32_t kNODE_CHILD = 0xf74c51eeu;
constexpr uint32_t kNODE_SIBLING = 0xb8990880u;
constexpr uint32_t kNODE_END = 0xa93eb864u;
constexpr uint32_t kFILE_END = 0xe3bb47f1u;
constexpr uint32_t kMAX_COUNT = 1u << 21;

struct BinCursor {
    const uint8_t* data;
    size_t size;
    size_t at = 0;
    bool ok = true;
    std::string error;

    BinCursor(const uint8_t* d, size_t s, size_t start = 0) : data(d), size(s), at(start) {}

    bool has(size_t n) {
        if (!ok) return false;
        if (at > size || n > size - at) {
            ok = false;
            error = "Unexpected EOF in DOCB stream at byte " + std::to_string(at) + " (need " + std::to_string(n) + ")";
            return false;
        }
        return true;
    }

    uint32_t u32() {
        uint32_t v = 0;
        if (has(4)) { std::memcpy(&v, data + at, 4); at += 4; }
        return v;
    }

    uint16_t u16() {
        uint16_t v = 0;
        if (has(2)) { std::memcpy(&v, data + at, 2); at += 2; }
        return v;
    }

    float f32() {
        float v = 0.0f;
        if (has(4)) { std::memcpy(&v, data + at, 4); at += 4; }
        return v;
    }

    uint32_t peek_u32() const {
        uint32_t v = 0;
        if (ok && at <= size && size - at >= 4) std::memcpy(&v, data + at, 4);
        return v;
    }

    void skip(size_t n) {
        if (has(n)) at += n;
    }

    uint32_t count(size_t elem_size) {
        uint32_t n = u32();
        if (ok && (n > kMAX_COUNT || !has(static_cast<size_t>(n) * elem_size))) {
            ok = false;
            error = "DOCB count out of bounds (" + std::to_string(n) + " x " + std::to_string(elem_size) + " at " + std::to_string(at) + ")";
            return 0;
        }
        return ok ? n : 0;
    }

    std::string text() {
        uint16_t n = u16();
        std::string s;
        if (n > 0 && has(n)) {
            size_t len = (data[at + n - 1] == 0) ? (n - 1) : n;
            s.assign(reinterpret_cast<const char*>(data + at), len);
            at += n;
        }
        return s;
    }
};

struct RawDocbMaterial {
    std::string name;
    float floats[17] = {};
    std::string inline_tex;
    uint32_t tex_a = 0, tex_b = 0;
    bool has_end_tag = false;
};

bool read_docb_material(BinCursor& c, RawDocbMaterial& out) {
    uint32_t tag = c.u32();
    if (!c.ok || tag != kMATERIAL) {
        c.ok = false;
        c.error = "Expected DOCB material tag 0x9709513e at byte " + std::to_string(c.at >= 4 ? c.at - 4 : 0);
        return false;
    }
    out.name = c.text();
    for (int i = 0; i < 17; ++i) out.floats[i] = c.f32();
    if (c.peek_u32() == kTEXTURE) {
        c.u32();
        out.inline_tex = c.text();
        out.tex_a = c.u32();
        out.tex_b = c.u32();
    }
    if (c.peek_u32() == kMATERIAL_END) {
        c.u32();
        out.has_end_tag = true;
    }
    return c.ok;
}

Material to_scene_material(const RawDocbMaterial& rm, uint32_t flags) {
    Material m;
    m.name = rm.name.empty() ? "docb_material" : rm.name;
    if (to_lower(rm.name).size() > 9 && to_lower(rm.name).rfind(".material") == rm.name.size() - 9) {
        m.material_file_ref = rm.name;
    }
    m.diffuse = {rm.floats[0], rm.floats[1], rm.floats[2], rm.floats[3]};
    m.specular = {rm.floats[4], rm.floats[5], rm.floats[6], rm.floats[7]};
    m.specular_power = rm.floats[8];
    m.emissive = {rm.floats[9], rm.floats[10], rm.floats[11], rm.floats[12]};
    m.ambient = {rm.floats[13], rm.floats[14], rm.floats[15], rm.floats[16]};
    if (!rm.inline_tex.empty()) {
        m.diffuse_tex.original_ref = rm.inline_tex;
        m.diffuse_tex.is_srgb = true;
    }
    if (m.diffuse.w < 0.999f) m.has_alpha = true;
    if ((flags & 0x0200u) != 0) m.double_sided = true;
    return m;
}

struct DocbPose {
    float frame = 0.0f;
    uint32_t flags = 0;
    Quat bz_q = {1, 0, 0, 0};
    Vec3 pos = {0, 0, 0};
};

struct DocbNodeSurf {
    uint32_t node_id = 0;
    uint32_t vert_count = 0;
    uint32_t index_count = 0;
    uint32_t first_tri = 0;
    RawDocbMaterial mat;
};

struct DocbRawNode {
    std::string name;
    uint32_t index = 0;
    int parent_raw = -1;
    uint32_t rigid_flag = 0;
    uint32_t flags = 0;
    Mat4 local_matrix = Mat4::identity();
    std::vector<uint32_t> colors;
    struct V { Vec3 p, n; Vec2 uv; };
    std::vector<V> verts;
    std::vector<DocbNodeSurf> surfs;
    std::vector<uint16_t> indices;
};

Vec4 unpack_argb(uint32_t c) {
    // D3DCOLOR is 0xAARRGGBB
    float a = ((c >> 24) & 0xFF) / 255.0f;
    float r = ((c >> 16) & 0xFF) / 255.0f;
    float g = ((c >> 8) & 0xFF) / 255.0f;
    float b = (c & 0xFF) / 255.0f;
    return {r, g, b, a};
}

} // namespace

bool parse_msh_bytes(const std::vector<uint8_t>& bytes, const std::filesystem::path& source_path,
                     Scene& out_scene, std::string& out_error, const MshParseOptions& opts) {
    out_scene = Scene{};
    out_scene.source_path = utf8_from_path(source_path);
    out_scene.source_name = utf8_from_path(source_path.stem());
    out_scene.uv_origin_bottom_left = opts.flip_uv_v;
    if (!bytes.empty()) out_scene.source_sha256 = sha256_hex(bytes.data(), bytes.size());

    auto probe = probe_bytes(bytes, source_path);
    if (!probe.supported || (probe.format != SourceFormat::MSH_DOCB_V1_BZCC && probe.format != SourceFormat::MSH_DOCB_V1_BZ2)) {
        out_error = probe.detail;
        return false;
    }
    out_scene.source_format = probe.format;

    uint32_t body_len = 0;
    std::memcpy(&body_len, bytes.data() + 48, 4);
    BinCursor c(bytes.data(), std::min(bytes.size(), static_cast<size_t>(52) + body_len), 52);

    std::string root_name = c.text();
    float radius = c.f32();
    (void)radius;
    Mat4 header_mat;
    for (float& v : header_mat.m) v = c.f32();
    Vec3 half_ext{c.f32(), c.f32(), c.f32()};
    uint32_t w7[7];
    for (uint32_t& w : w7) w = c.u32();
    float scale_f = 1.0f;
    std::memcpy(&scale_f, &w7[1], 4);
    if (std::isfinite(scale_f) && scale_f > 1e-6f && scale_f < 1e6f) out_scene.source_scale = scale_f;

    // Combined geometry arrays
    uint32_t nP = c.count(12);
    std::vector<Vec3> P(nP);
    for (auto& v : P) { v.x = c.f32(); v.y = c.f32(); v.z = c.f32(); }

    uint32_t nN = c.count(12);
    std::vector<Vec3> N(nN);
    for (auto& v : N) { v.x = c.f32(); v.y = c.f32(); v.z = c.f32(); }

    uint32_t nT = c.count(8);
    std::vector<Vec2> T(nT);
    for (auto& v : T) {
        v.x = c.f32();
        float tv = c.f32();
        v.y = opts.flip_uv_v ? (1.0f - tv) : tv;
    }

    uint32_t nC = c.count(4);
    std::vector<uint32_t> C(nC);
    for (auto& v : C) v = c.u32();

    uint32_t nF = c.count(20);
    std::vector<std::array<uint16_t, 10>> F(nF);
    for (auto& f : F) for (auto& w : f) w = c.u16();

    uint32_t nS = c.count(16);
    std::vector<std::pair<uint32_t, RawDocbMaterial>> combined_mats(nS);
    for (uint32_t s = 0; s < nS; ++s) {
        uint32_t fl = c.u32();
        c.skip(8); // vc, ic
        combined_mats[s].first = fl;
        if (!read_docb_material(c, combined_mats[s].second)) { out_error = c.error; return false; }
    }

    // Per-position bone weights
    uint32_t nW = c.count(4);
    std::vector<std::vector<std::pair<float, uint16_t>>> W(nW);
    uint32_t max_influences = 0;
    for (uint32_t i = 0; i < nW; ++i) {
        uint32_t k = c.count(6);
        max_influences = std::max(max_influences, k);
        W[i].reserve(k);
        for (uint32_t j = 0; j < k; ++j) {
            float w = c.f32();
            uint16_t bi = c.u16();
            W[i].emplace_back(w, bi);
        }
    }

    // Combined surfaces, indices, face planes
    uint32_t nR = c.count(20);
    for (uint32_t r = 0; r < nR; ++r) {
        c.skip(16);
        RawDocbMaterial dummy;
        if (!read_docb_material(c, dummy)) { out_error = c.error; return false; }
    }
    uint32_t nSurfIdx = c.count(2);
    c.skip(static_cast<size_t>(nSurfIdx) * 2);
    uint32_t nPlanes = c.count(16);
    c.skip(static_cast<size_t>(nPlanes) * 16);

    // Inverse bind matrices (bones)
    uint32_t nB = c.count(64);
    std::vector<Mat4> bones(nB);
    for (auto& bm : bones) for (float& v : bm.m) v = c.f32();

    // Node rest poses
    uint32_t nPoses = c.count(36);
    std::vector<DocbPose> poses(nPoses);
    for (auto& p : poses) {
        p.frame = c.f32();
        p.flags = c.u32();
        p.bz_q.w = c.f32(); p.bz_q.x = c.f32(); p.bz_q.y = c.f32(); p.bz_q.z = c.f32();
        p.pos.x = c.f32(); p.pos.y = c.f32(); p.pos.z = c.f32();
    }

    // Baked animation clips
    uint32_t nAnim = c.count(12);
    struct RawMshAnim {
        std::string name;
        float max_frame = 0.0f;
        float end_frame = 0.0f;
        struct Track {
            uint32_t node_id = 0;
            std::vector<DocbPose> keys;
        };
        std::vector<Track> tracks;
    };
    std::vector<RawMshAnim> raw_anims;
    for (uint32_t ai = 0; ai < nAnim; ++ai) {
        RawMshAnim anim;
        anim.name = c.text();
        uint32_t atype = c.u32(); (void)atype;
        anim.max_frame = c.f32();
        anim.end_frame = c.f32();
        uint32_t nStates = c.count(36);
        c.skip(static_cast<size_t>(nStates) * 36);
        uint32_t nTracks = c.count(12);
        anim.tracks.reserve(nTracks);
        for (uint32_t ti = 0; ti < nTracks; ++ti) {
            RawMshAnim::Track tr;
            tr.node_id = c.u32();
            float tmax = c.f32(); (void)tmax;
            uint32_t nKeys = c.count(36);
            tr.keys.resize(nKeys);
            for (auto& k : tr.keys) {
                k.frame = c.f32();
                k.flags = c.u32();
                k.bz_q.w = c.f32(); k.bz_q.x = c.f32(); k.bz_q.y = c.f32(); k.bz_q.z = c.f32();
                k.pos.x = c.f32(); k.pos.y = c.f32(); k.pos.z = c.f32();
            }
            anim.tracks.push_back(std::move(tr));
        }
        raw_anims.push_back(std::move(anim));
    }

    if (!c.ok) { out_error = c.error; return false; }

    // Optional Classic BZ2 MRM progressive LOD table
    if (c.peek_u32() == kMRM_BLOCK) {
        c.u32();
        uint32_t mv = c.u32(), mf = c.u32(), mn = c.u32(), mt = c.u32(), nupd = c.u32();
        (void)mf; (void)mn; (void)mt;
        c.skip(static_cast<size_t>(mv) * 24 + static_cast<size_t>(nupd) * 16);
        out_scene.add_diag(DiagSeverity::Info, "MSH_CLASSIC_MRM_SKIPPED",
            "Classic BZ2 MRM progressive-LOD block detected and skipped cleanly (full-resolution geometry preserved)");
    }

    // Parse full node hierarchy tree
    std::vector<DocbRawNode> raw_nodes;
    bool parse_tree_ok = true;
    auto read_node_list = [&](auto&& self, int parent_raw_idx, int depth) -> void {
        if (depth > 256 || !c.ok) { parse_tree_ok = false; return; }
        while (c.ok) {
            DocbRawNode rn;
            rn.name = c.text();
            rn.index = c.u32();
            rn.rigid_flag = c.u32();
            rn.flags = c.u32();
            for (float& v : rn.local_matrix.m) v = c.f32();
            uint32_t nCol = c.count(4);
            rn.colors.resize(nCol);
            for (auto& col : rn.colors) col = c.u32();
            uint32_t npl = c.count(16);
            c.skip(static_cast<size_t>(npl) * 16);
            uint32_t nv = c.count(32);
            rn.verts.resize(nv);
            for (auto& v : rn.verts) {
                v.p.x = c.f32(); v.p.y = c.f32(); v.p.z = c.f32();
                v.n.x = c.f32(); v.n.y = c.f32(); v.n.z = c.f32();
                v.uv.x = c.f32();
                float tv = c.f32();
                v.uv.y = opts.flip_uv_v ? (1.0f - tv) : tv;
            }
            uint32_t ns = c.count(20);
            rn.surfs.resize(ns);
            for (uint32_t s = 0; s < ns; ++s) {
                rn.surfs[s].node_id = c.u32();
                rn.surfs[s].vert_count = c.u32();
                rn.surfs[s].index_count = c.u32();
                rn.surfs[s].first_tri = c.u32();
                if (!read_docb_material(c, rn.surfs[s].mat)) { parse_tree_ok = false; return; }
            }
            uint32_t ni = c.count(2);
            rn.indices.resize(ni);
            for (auto& idx : rn.indices) idx = c.u16();
            rn.parent_raw = parent_raw_idx;
            int my_raw = static_cast<int>(raw_nodes.size());
            raw_nodes.push_back(std::move(rn));

            uint32_t pk = c.peek_u32();
            if (pk == kNODE_CHILD) {
                c.u32();
                self(self, my_raw, depth + 1);
                pk = c.peek_u32();
            }
            if (pk == kNODE_END) {
                c.u32();
                pk = c.peek_u32();
            }
            if (pk == kNODE_SIBLING) {
                c.u32();
                continue;
            }
            return;
        }
    };
    read_node_list(read_node_list, -1, 0);
    if (!c.ok || !parse_tree_ok) {
        out_error = c.error.empty() ? "Failed parsing DOCB node tree" : c.error;
        return false;
    }
    if (c.peek_u32() == kFILE_END) c.u32();

    // Helper: deduplicate materials in scene
    std::unordered_map<std::string, int> mat_lookup;
    auto register_material = [&](const RawDocbMaterial& rm, uint32_t flags) -> int {
        std::string key = rm.name + "|" + rm.inline_tex + "|" + std::to_string((flags & 0x0200u) != 0);
        auto it = mat_lookup.find(key);
        if (it != mat_lookup.end()) return it->second;
        int idx = static_cast<int>(out_scene.materials.size());
        out_scene.materials.push_back(to_scene_material(rm, flags));
        mat_lookup.emplace(std::move(key), idx);
        return idx;
    };

    // Build Scene nodes sorted/mapped by their DOCB node index
    std::unordered_map<uint32_t, int> docb_id_to_scene_node;
    out_scene.nodes.reserve(raw_nodes.size());
    for (size_t i = 0; i < raw_nodes.size(); ++i) {
        const auto& rn = raw_nodes[i];
        Node node;
        node.raw_name = rn.name;
        node.name = rn.name.empty() ? ("node_" + std::to_string(rn.index)) : rn.name;
        // Deduplicate node names if needed
        std::string base = node.name;
        int suffix = 1;
        while (out_scene.find_node(node.name) >= 0) node.name = base + "_" + std::to_string(suffix++);
        node.parent_index = rn.parent_raw;
        node.local_matrix = rn.local_matrix;
        if (rn.index < bones.size()) {
            node.inverse_bind_matrix = bones[rn.index];
            node.has_inverse_bind = true;
        }
        if (rn.index < poses.size()) {
            node.base_pose_matrix = Mat4::from_bz_trs(poses[rn.index].pos, poses[rn.index].bz_q);
            node.has_base_pose = true;
        }
        std::string lo = to_lower(node.name);
        if (lo.rfind("hp_", 0) == 0 || lo == "hp") { node.is_hardpoint = true; node.is_hidden = true; }
        if (lo.rfind("__c", 0) != std::string::npos || lo.find("__c") != std::string::npos) node.is_collision = true;
        if (lo.find("__h") != std::string::npos) node.is_hidden = true;

        int sn_idx = static_cast<int>(out_scene.nodes.size());
        out_scene.nodes.push_back(std::move(node));
        docb_id_to_scene_node[rn.index] = sn_idx;
        if (rn.parent_raw >= 0) out_scene.nodes[rn.parent_raw].children.push_back(sn_idx);
        else out_scene.root_nodes.push_back(sn_idx);
    }

    // Decide whether per-node rigid meshes exist in the node tree OR geometry is skinned/combined
    size_t node_mesh_tris = 0;
    for (const auto& rn : raw_nodes) node_mesh_tris += rn.indices.size() / 3;

    bool is_skinned_model = (max_influences > 1) || (node_mesh_tris == 0 && nF > 0);
    // Even if max_influences == 1, if w7[2]==0 and node_mesh_tris==0, geometry is in combined buffers (e.g., ivcons_startup, fvtank_skel)
    if (!is_skinned_model && node_mesh_tris > 0) {
        // Build per-node rigid meshes preserving full articulated hierarchy!
        for (size_t i = 0; i < raw_nodes.size(); ++i) {
            const auto& rn = raw_nodes[i];
            if (rn.verts.empty() || rn.indices.empty()) continue;
            Mesh mesh;
            mesh.name = out_scene.nodes[i].name;
            mesh.has_normals = true;
            mesh.has_uvs = true;
            mesh.has_vertex_colors = (rn.colors.size() == rn.verts.size() && !rn.colors.empty());
            mesh.vertices.resize(rn.verts.size());
            for (size_t vi = 0; vi < rn.verts.size(); ++vi) {
                mesh.vertices[vi].pos = rn.verts[vi].p;
                mesh.vertices[vi].normal = rn.verts[vi].n.normalized();
                mesh.vertices[vi].uv = rn.verts[vi].uv;
                if (mesh.has_vertex_colors) mesh.vertices[vi].color = unpack_argb(rn.colors[vi]);
                mesh.vertices[vi].source_vertex_index = static_cast<int>(vi);
            }
            // Each surface in rn.surfs has its own vertex slice (vert_count) and index slice (index_count) starting from 0!
            size_t vert_base = 0;
            size_t idx_base = 0;
            for (const auto& sf : rn.surfs) {
                int mat_idx = register_material(sf.mat, rn.flags);
                size_t ic = sf.index_count;
                if (idx_base + ic > rn.indices.size()) ic = (rn.indices.size() > idx_base) ? (rn.indices.size() - idx_base) : 0;
                for (size_t k = 0; k + 2 < ic; k += 3) {
                    uint32_t i0 = static_cast<uint32_t>(vert_base + rn.indices[idx_base + k]);
                    uint32_t i1 = static_cast<uint32_t>(vert_base + rn.indices[idx_base + k + 1]);
                    uint32_t i2 = static_cast<uint32_t>(vert_base + rn.indices[idx_base + k + 2]);
                    if (i0 < mesh.vertices.size() && i1 < mesh.vertices.size() && i2 < mesh.vertices.size()) {
                        mesh.indices.push_back(i0);
                        mesh.indices.push_back(i1);
                        mesh.indices.push_back(i2);
                        mesh.triangle_materials.push_back(mat_idx);
                    }
                }
                vert_base += sf.vert_count;
                idx_base += sf.index_count;
            }
            mesh.source_polygon_count = mesh.indices.size() / 3;
            if (!mesh.indices.empty()) {
                int mi = static_cast<int>(out_scene.meshes.size());
                out_scene.meshes.push_back(std::move(mesh));
                out_scene.nodes[i].mesh_index = mi;
            }
        }
    } else if (nF > 0 && nP > 0) {
        // Skinned or combined mesh in DOCB header buffers (MeshLod.cpp verified layout!)
        std::vector<Mat4> inv_bones(nB, Mat4::identity());
        for (uint32_t b = 0; b < nB; ++b) {
            if (!bones[b].invert_affine(inv_bones[b])) {
                out_scene.add_diag(DiagSeverity::Warning, "MSH_SINGULAR_BONE",
                    "Bone matrix " + std::to_string(b) + " is singular; using identity fallback");
            }
        }
        // Check whether combined positions P are in bone-local space or already in model space via header bbox
        std::vector<Vec3> boned_P(nP, Vec3{0, 0, 0});
        for (uint32_t p = 0; p < nP; ++p) {
            if (p < W.size() && !W[p].empty()) {
                for (const auto& [wt, bi] : W[p]) {
                    if (bi < nB) boned_P[p] = boned_P[p] + inv_bones[bi].transform_point(P[p]) * wt;
                }
            } else {
                boned_P[p] = P[p];
            }
        }
        BBox raw_box, boned_box;
        for (const auto& f : F) {
            for (int k = 0; k < 3; ++k) {
                if (f[1 + k] < nP) { raw_box.expand(P[f[1 + k]]); boned_box.expand(boned_P[f[1 + k]]); }
            }
        }
        auto box_err = [&](const BBox& bx) {
            Vec3 ctr = bx.center();
            Vec3 ext = bx.extent() * 0.5f;
            float worst = 0.0f;
            float sz = std::max({0.01f, half_ext.x, half_ext.y, half_ext.z});
            worst = std::max(worst, std::fabs(ctr.x - header_mat.m[12]));
            worst = std::max(worst, std::fabs(ctr.y - header_mat.m[13]));
            worst = std::max(worst, std::fabs(ctr.z - header_mat.m[14]));
            worst = std::max(worst, std::fabs(ext.x - half_ext.x));
            worst = std::max(worst, std::fabs(ext.y - half_ext.y));
            worst = std::max(worst, std::fabs(ext.z - half_ext.z));
            return worst / sz;
        };
        bool use_bones = (box_err(boned_box) <= box_err(raw_box));

        const bool one_list = (nC == nP) && (nN == nP) && (nT == nP);
        const int norm_at = one_list ? 1 : 4;
        const int uv_at = one_list ? 1 : 7;

        std::vector<int> mat_map(nS, -1);
        for (uint32_t s = 0; s < nS; ++s) mat_map[s] = register_material(combined_mats[s].second, combined_mats[s].first);

        Mesh mesh;
        mesh.name = root_name.empty() ? out_scene.source_name : root_name;
        mesh.has_normals = (nN > 0);
        mesh.has_uvs = (nT > 0);
        mesh.has_vertex_colors = (nC == nP && nC > 0);
        mesh.is_skinned = (nW == nP && nB > 0);
        mesh.vertices_in_model_space = true;

        std::unordered_map<uint64_t, uint32_t> dedup;
        for (const auto& f : F) {
            if (f[0] >= nS) continue;
            int mat_idx = mat_map[f[0]];
            bool ok_face = true;
            for (int k = 0; k < 3; ++k) {
                if (f[1 + k] >= nP) ok_face = false;
                if (mesh.has_normals && f[norm_at + k] >= nN) ok_face = false;
                if (mesh.has_uvs && f[uv_at + k] >= nT) ok_face = false;
            }
            if (!ok_face) continue;

            for (int k = 0; k < 3; ++k) {
                uint16_t pi = f[1 + k];
                uint16_t ni = mesh.has_normals ? f[norm_at + k] : 0;
                uint16_t ti = mesh.has_uvs ? f[uv_at + k] : 0;
                uint64_t key = uint64_t(pi) | (uint64_t(ni) << 16) | (uint64_t(ti) << 32);
                auto it = dedup.find(key);
                if (it == dedup.end()) {
                    MeshVertex mv;
                    mv.source_vertex_index = pi;
                    if (use_bones) {
                        mv.pos = boned_P[pi];
                        if (mesh.has_normals) {
                            Vec3 nsum{0, 0, 0};
                            if (pi < W.size() && !W[pi].empty()) {
                                for (const auto& [wt, bi] : W[pi])
                                    if (bi < nB) nsum = nsum + inv_bones[bi].transform_vector(N[ni]) * wt;
                            } else {
                                nsum = N[ni];
                            }
                            mv.normal = nsum.normalized();
                        }
                    } else {
                        mv.pos = P[pi];
                        if (mesh.has_normals) mv.normal = N[ni].normalized();
                    }
                    if (mesh.has_uvs) mv.uv = T[ti];
                    if (mesh.has_vertex_colors) mv.color = unpack_argb(C[pi]);
                    uint32_t new_vi = static_cast<uint32_t>(mesh.vertices.size());
                    mesh.vertices.push_back(mv);
                    if (mesh.is_skinned) {
                        std::vector<BoneWeight> bws;
                        for (const auto& [wt, bi] : W[pi]) {
                            auto bit = docb_id_to_scene_node.find(bi);
                            if (bit != docb_id_to_scene_node.end()) {
                                bws.push_back({bit->second, wt});
                                out_scene.nodes[bit->second].is_bone = true;
                            }
                        }
                        mesh.skin_weights.push_back(std::move(bws));
                    }
                    it = dedup.emplace(key, new_vi).first;
                }
                mesh.indices.push_back(it->second);
            }
            mesh.triangle_materials.push_back(mat_idx);
        }
        mesh.source_polygon_count = mesh.indices.size() / 3;
        if (!mesh.indices.empty()) {
            int mi = static_cast<int>(out_scene.meshes.size());
            out_scene.meshes.push_back(std::move(mesh));
            if (!out_scene.nodes.empty()) out_scene.nodes[0].mesh_index = mi;
        }
    }

    // Mark all ancestor nodes of bones as bones so skeleton chains stay unbroken
    for (size_t i = 0; i < out_scene.nodes.size(); ++i) {
        if (out_scene.nodes[i].is_bone) {
            int p = out_scene.nodes[i].parent_index;
            while (p >= 0) { out_scene.nodes[p].is_bone = true; p = out_scene.nodes[p].parent_index; }
        }
    }

    // Convert baked MSH animation clips to canonical AnimationClips
    if (opts.include_animations) {
        for (const auto& ra : raw_anims) {
            AnimationClip clip;
            clip.name = ra.name.empty() ? "msh_anim" : ra.name;
            clip.fps = 20.0f;
            clip.start_frame = 0.0f;
            clip.end_frame = std::max(ra.max_frame, ra.end_frame);
            for (const auto& rtr : ra.tracks) {
                auto it = docb_id_to_scene_node.find(rtr.node_id);
                if (it == docb_id_to_scene_node.end()) continue;
                int sn_idx = it->second;
                AnimTrack tr;
                tr.node_index = sn_idx;
                tr.node_name = out_scene.nodes[sn_idx].name;
                for (const auto& k : rtr.keys) {
                    if ((k.flags & 0x1u) != 0) tr.translations.push_back({k.frame, k.pos});
                    if ((k.flags & 0x2u) != 0) {
                        // DOCB quaternion maps to row-vector matrix via R_col(q) => column-vector quat is conjugate(q)
                        tr.rotations.push_back({k.frame, k.bz_q.conjugate().normalized()});
                    }
                }
                if (!tr.translations.empty() || !tr.rotations.empty())
                    clip.tracks.push_back(std::move(tr));
            }
            if (!clip.tracks.empty()) out_scene.animations.push_back(std::move(clip));
        }
    }

    out_scene.update_world_matrices();
    out_scene.normalize_skin_weights();
    out_scene.ensure_inverse_bind_matrices();

    if (out_scene.meshes.empty()) {
        out_scene.add_diag(DiagSeverity::Warning, "MSH_NO_GEOMETRY", "DOCB file parsed cleanly but contains no mesh geometry");
    }
    return true;
}

bool parse_msh_file(const std::filesystem::path& path, Scene& out_scene, std::string& out_error,
                    const MshParseOptions& opts) {
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(path, bytes, out_error)) return false;
    return parse_msh_bytes(bytes, path, out_scene, out_error, opts);
}

} // namespace bz
