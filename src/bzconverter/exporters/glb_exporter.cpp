#include "bzconverter/exporters/glb_exporter.hpp"
#include "bzconverter/core/json.hpp"
#include "bzconverter/core/output_bundle.hpp"
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <algorithm>
#include <functional>
// Real image decode validation (stb_image v2.34, public domain) instead of
// structural marker sniffing: a texture is valid only if it fully decodes.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_PSD
#define STBI_NO_TGA
#include "bzconverter/core/third_party/stb_image.h"

namespace bz {

// Dedup by mapped joint, sort descending with deterministic joint-index
// tie-break, cap to the *exact* configured limit, then renormalize the kept
// influences only. Shared by both public pack helpers and the multi-set path.
std::vector<SkinInfluence> select_skin_influences(const std::vector<BoneWeight>& wl,
                                                  const std::unordered_map<int, uint16_t>& node_to_joint,
                                                  size_t limit) {
    std::vector<SkinInfluence> inf;
    inf.reserve(wl.size());
    for (const auto& bw : wl) {
        auto it = node_to_joint.find(bw.bone_node_index);
        if (it == node_to_joint.end() || !std::isfinite(bw.weight) || !(bw.weight > 1e-6f)) continue;
        bool found = false;
        for (auto& e : inf) {
            if (e.joint == it->second) { e.weight += bw.weight; found = true; break; }
        }
        if (!found) inf.push_back({it->second, bw.weight});
    }
    std::sort(inf.begin(), inf.end(), [](const SkinInfluence& a, const SkinInfluence& b) {
        if (a.weight != b.weight) return a.weight > b.weight;
        return a.joint < b.joint;
    });
    if (limit > 0 && inf.size() > limit) inf.resize(limit);
    float sum = 0.0f;
    for (const auto& e : inf) sum += e.weight;
    if (sum > 1e-6f) {
        for (auto& e : inf) e.weight /= sum;
    } else {
        inf.clear();
    }
    return inf;
}

void write_skin_slots(const std::vector<SkinInfluence>& inf, uint16_t* j, float* w, size_t slots) {
    for (size_t s = 0; s < slots; ++s) { j[s] = 0; w[s] = 0.0f; }
    for (size_t k = 0; k < inf.size() && k < slots; ++k) {
        j[k] = inf[k].joint;
        w[k] = inf[k].weight;
    }
}

void pack_skin_influences(const std::vector<BoneWeight>& wl,
                          const std::unordered_map<int, uint16_t>& node_to_joint,
                          uint16_t j[4], float w[4]) {
    write_skin_slots(select_skin_influences(wl, node_to_joint, 4), j, w, 4);
}

void pack_skin_influences_ext(const std::vector<BoneWeight>& wl,
                              const std::unordered_map<int, uint16_t>& node_to_joint,
                              uint16_t* j, float* w, size_t slots) {
    write_skin_slots(select_skin_influences(wl, node_to_joint, 0), j, w, slots);
}

namespace {

size_t align4(std::vector<uint8_t>& buf) {
    while (buf.size() % 4 != 0) buf.push_back(0);
    return buf.size();
}

// Real image validation: the file must actually DECODE (stb_image does the
// full entropy/palette/interlace work), and the decoded size must be sane.
// Marker sniffing accepted undecodable files and rejected valid paletted
// PNGs; this contract replaces both. glTF 2.0 core allows PNG and JPEG only.
bool validate_image_bytes(const std::vector<uint8_t>& b, std::string& mime) {
    if (b.size() < 16) return false;
    if (b.size() > 256u * 1024u * 1024u) return false;
    static const uint8_t png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    bool is_png = b.size() >= 8 && std::memcmp(b.data(), png_sig, 8) == 0;
    bool is_jpeg = b[0] == 0xFF && b[1] == 0xD8;
    if (!is_png && !is_jpeg) return false;
    int w = 0, h = 0, comp = 0;
    // Preflight BEFORE any decode: a small PNG/JPEG can declare gigantic
    // dimensions and make the full decoder allocate huge amounts of RAM.
    // stbi_info_from_memory reads only headers; enforce the same limits the
    // post-decode path enforces, before any pixel bytes are decompressed.
    if (!stbi_info_from_memory(b.data(), static_cast<int>(b.size()), &w, &h, &comp)) return false;
    const int kMaxDim = 32768;                 // per side
    const double kMaxPixels = 268435456.0;     // 256 MP guard
    if (w <= 0 || h <= 0 || w > kMaxDim || h > kMaxDim) return false;
    if (double(w) * double(h) > kMaxPixels) return false;
    const size_t decode_bytes = size_t(w) * size_t(h) * 4; // RGBA
    if (decode_bytes > size_t(1) << 30) return false;      // 1 GiB decode budget
    // stbi_load_from_memory with 4 requested channels handles baseline and
    // progressive JPEG, indexed/interlaced PNG, tRNS transparency; anything
    // that does not fully decode returns null.
    stbi_uc* pixels = stbi_load_from_memory(b.data(), static_cast<int>(b.size()), &w, &h, &comp, 4);
    if (!pixels) return false;
    STBI_FREE(pixels);
    if (w <= 0 || h <= 0 || w > kMaxDim || h > kMaxDim) return false;
    if (double(w) * double(h) > kMaxPixels) return false; // 256 MP guard
    mime = is_png ? "image/png" : "image/jpeg";
    return true;
}

int add_buffer_view(Json& bvs, std::vector<uint8_t>& bin, const void* data, size_t len, int target = 0) {
    size_t off = align4(bin);
    if (len > 0) {
        const auto* p = reinterpret_cast<const uint8_t*>(data);
        bin.insert(bin.end(), p, p + len);
    }
    align4(bin);
    Json bv = Json::object();
    bv["buffer"] = 0;
    bv["byteOffset"] = off;
    bv["byteLength"] = len;
    if (target != 0) bv["target"] = target;
    int idx = static_cast<int>(bvs.as_arr().size());
    bvs.push_back(std::move(bv));
    return idx;
}

int add_accessor(Json& accs, int bv, int comp_type, size_t count, const std::string& type,
                 const std::vector<float>* vmin = nullptr, const std::vector<float>* vmax = nullptr) {
    Json a = Json::object();
    a["bufferView"] = bv;
    a["componentType"] = comp_type;
    a["count"] = count;
    a["type"] = type;
    if (vmin && !vmin->empty()) {
        Json mn = Json::array();
        for (float f : *vmin) mn.push_back(f);
        a["min"] = std::move(mn);
    }
    if (vmax && !vmax->empty()) {
        Json mx = Json::array();
        for (float f : *vmax) mx.push_back(f);
        a["max"] = std::move(mx);
    }
    int idx = static_cast<int>(accs.as_arr().size());
    accs.push_back(std::move(a));
    return idx;
}

} // namespace

bool export_scene_gltf(const Scene& scene, const std::filesystem::path& out_path,
                       bool binary_glb, const ExportOptions& opts,
                       std::vector<Diagnostic>& diags, std::string& err,
                       std::atomic<bool>* cancel_flag) {
    Json gltf = Json::object();
    Json asset = Json::object();
    asset["version"] = "2.0";
    #ifndef BZCONV_VERSION
#define BZCONV_VERSION "0.0.1"
#endif
    asset["generator"] = std::string("BZ_XSI_MSH_FBX_CONVERTER ") + BZCONV_VERSION;
    gltf["asset"] = std::move(asset);

    std::vector<uint8_t> bin;
    Json buffer_views = Json::array();
    Json accessors = Json::array();
    Json images = Json::array();
    Json textures = Json::array();
    Json samplers = Json::array();
    Json materials = Json::array();
    Json meshes = Json::array();
    Json nodes = Json::array();
    Json skins = Json::array();
    Json animations = Json::array();

    {
        Json s = Json::object();
        s["magFilter"] = 9729; // LINEAR
        s["minFilter"] = 9987; // LINEAR_MIPMAP_LINEAR
        s["wrapS"] = 10497;    // REPEAT
        s["wrapT"] = 10497;
        samplers.push_back(std::move(s));
    }

    // Every artifact of this export is declared before any file is written.
    OutputBundle bundle;

    // Deduplicate embedded/referenced textures
    std::unordered_map<std::string, int> tex_cache;
    auto add_texture = [&](const TextureChannel& ch) -> int {
        if (!opts.include_textures) return -1;
        std::string p = !ch.converted_png_path.empty() ? ch.converted_png_path : ch.resolved_path;
        if (p.empty()) return -1;
        auto it = tex_cache.find(p);
        if (it != tex_cache.end()) return it->second;

        std::vector<uint8_t> img_bytes;
        std::string rerr;
        if (!read_file_bytes(path_from_utf8(p), img_bytes, rerr)) {
            diags.push_back({DiagSeverity::Error, "GLTF_TEXTURE_MISSING",
                "Requested texture '" + p + "' cannot be read: " + rerr});
            return -1;
        }
        std::string mime;
        if (!validate_image_bytes(img_bytes, mime)) {
            diags.push_back({DiagSeverity::Error, "GLTF_TEXTURE_INVALID",
                "Requested texture '" + p + "' is corrupt or unsupported in glTF 2.0"});
            return -1;
        }

        int img_idx = static_cast<int>(images.as_arr().size());
        Json img = Json::object();
        if (binary_glb || opts.embed_textures) {
            int bv = add_buffer_view(buffer_views, bin, img_bytes.data(), img_bytes.size(), 0);
            img["bufferView"] = bv;
            img["mimeType"] = mime;
        } else {
            auto tex_dir = out_path.parent_path() / (out_path.stem().wstring() + L"_textures");
            auto dst = tex_dir / path_from_utf8(p).filename();
            std::vector<uint8_t> tex_bytes;
            std::string terr;
            if (!read_file_bytes(path_from_utf8(p), tex_bytes, terr)) {
                diags.push_back({DiagSeverity::Warning, "GLTF_TEXTURE_STAGE_FAILED",
                    "Cannot stage texture '" + p + "': " + terr});
                return -1;
            }
            bundle.add(dst, std::move(tex_bytes), "glTF texture");
            img["uri"] = utf8_from_path(tex_dir.filename() / dst.filename());
        }
        images.push_back(std::move(img));

        int tex_idx = static_cast<int>(textures.as_arr().size());
        Json tex = Json::object();
        tex["sampler"] = 0;
        tex["source"] = img_idx;
        textures.push_back(std::move(tex));
        tex_cache[p] = tex_idx;
        return tex_idx;
    };

    // Materials
    if (opts.include_materials) {
        for (const auto& m : scene.materials) {
            Json jm = Json::object();
            jm["name"] = m.name.empty() ? "material" : m.name;
            jm["doubleSided"] = m.double_sided;
            if (m.has_alpha || m.diffuse.w < 0.999f) jm["alphaMode"] = "BLEND";

            Json pbr = Json::object();
            Json bcf = Json::array();
            bcf.push_back(std::clamp(m.diffuse.x, 0.0f, 1.0f));
            bcf.push_back(std::clamp(m.diffuse.y, 0.0f, 1.0f));
            bcf.push_back(std::clamp(m.diffuse.z, 0.0f, 1.0f));
            bcf.push_back(std::clamp(m.diffuse.w, 0.0f, 1.0f));
            pbr["baseColorFactor"] = std::move(bcf);
            // Derive meaningful roughness from Blinn-Phong specular power
            float rough = std::clamp(std::sqrt(2.0f / (std::max(1.0f, m.specular_power) + 2.0f)), 0.15f, 0.95f);
            pbr["metallicFactor"] = 0.0f;
            pbr["roughnessFactor"] = rough;

            int diff_t = add_texture(m.diffuse_tex);
            if (diff_t >= 0) {
                Json ti = Json::object(); ti["index"] = diff_t;
                pbr["baseColorTexture"] = std::move(ti);
            }
            jm["pbrMetallicRoughness"] = std::move(pbr);

            int norm_t = add_texture(m.normal_tex);
            if (norm_t >= 0) {
                Json ni = Json::object(); ni["index"] = norm_t;
                jm["normalTexture"] = std::move(ni);
            }
            int emis_t = add_texture(m.emissive_tex);
            if (emis_t >= 0) {
                Json ei = Json::object(); ei["index"] = emis_t;
                jm["emissiveTexture"] = std::move(ei);
                Json ef = Json::array(); ef.push_back(1.0f); ef.push_back(1.0f); ef.push_back(1.0f);
                jm["emissiveFactor"] = std::move(ef);
            } else if (m.emissive.x > 0.001f || m.emissive.y > 0.001f || m.emissive.z > 0.001f || m.emissive_flag) {
                Json ef = Json::array();
                if (m.emissive_flag) {
                    ef.push_back(std::clamp(m.diffuse.x, 0.0f, 1.0f));
                    ef.push_back(std::clamp(m.diffuse.y, 0.0f, 1.0f));
                    ef.push_back(std::clamp(m.diffuse.z, 0.0f, 1.0f));
                } else {
                    ef.push_back(std::clamp(m.emissive.x, 0.0f, 1.0f));
                    ef.push_back(std::clamp(m.emissive.y, 0.0f, 1.0f));
                    ef.push_back(std::clamp(m.emissive.z, 0.0f, 1.0f));
                }
                jm["emissiveFactor"] = std::move(ef);
            }

            // Preserve BZCC teamColor & specular refs in extras without polluting diffuse
            Json extras = Json::object();
            if (!m.team_color_tex.resolved_path.empty()) extras["bz_teamColorTexture"] = m.team_color_tex.resolved_path;
            if (!m.specular_tex.resolved_path.empty()) extras["bz_specularTexture"] = m.specular_tex.resolved_path;
            if (!m.material_file_ref.empty()) extras["bz_materialFile"] = m.material_file_ref;
            if (!extras.as_obj().empty()) jm["extras"] = std::move(extras);

            materials.push_back(std::move(jm));
        }
    }

    // Collect skin joints if any mesh is skinned
    std::vector<int> joint_nodes;
    std::unordered_map<int, uint16_t> node_to_joint;
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        if (scene.nodes[i].is_bone) {
            node_to_joint[static_cast<int>(i)] = static_cast<uint16_t>(joint_nodes.size());
            joint_nodes.push_back(static_cast<int>(i));
        }
    }
    int skin_idx = -1;
    if (!joint_nodes.empty()) {
        std::vector<float> ibms;
        ibms.reserve(joint_nodes.size() * 16);
        Json jarr = Json::array();
        for (int ni : joint_nodes) {
            jarr.push_back(ni);
            const Mat4& ibm = scene.nodes[ni].inverse_bind_matrix;
            // Row-vector Mat4::m memory layout equals column-major M_col = transpose(M_row)!
            for (float f : ibm.m) ibms.push_back(f);
        }
        int bv_ibm = add_buffer_view(buffer_views, bin, ibms.data(), ibms.size() * sizeof(float), 0);
        int acc_ibm = add_accessor(accessors, bv_ibm, 5126, joint_nodes.size(), "MAT4");
        Json sk = Json::object();
        sk["inverseBindMatrices"] = acc_ibm;
        sk["joints"] = std::move(jarr);
        // glTF skeleton is optional. Only set it when a single bone is a
        // root of the joint hierarchy; never point it at an arbitrary mesh.
        int skeleton_root = -1;
        for (int jn : joint_nodes) {
            if (scene.nodes[jn].parent_index < 0) {
                if (skeleton_root >= 0) { skeleton_root = -1; break; }
                skeleton_root = jn;
            }
        }
        if (skeleton_root >= 0) sk["skeleton"] = skeleton_root;
        skin_idx = 0;
        skins.push_back(std::move(sk));
    }

    // Canonical hierarchy repair, BEFORE any matrix resolution or JSON
    // serialization. parent_index is the single authority for the parent
    // edge; n.children is derived from it below. Ancestor cycles are cut at
    // the node the walk re-enters, so neither the world matrices nor the
    // exported node children can still contain a cycle afterwards.
    std::vector<int> repaired_parent(scene.nodes.size(), -1);
    for (size_t ni = 0; ni < scene.nodes.size(); ++ni) {
        int p = scene.nodes[ni].parent_index;
        if (p >= 0 && p < static_cast<int>(scene.nodes.size()) && p != static_cast<int>(ni)) {
            repaired_parent[ni] = p;
        } else if (p >= 0) {
            diags.push_back({DiagSeverity::Warning, "GLB_INVALID_PARENT",
                "Node '" + scene.nodes[ni].name + "' has invalid parent index " + std::to_string(p)});
        }
    }
    for (size_t start = 0; start < scene.nodes.size(); ++start) {
        std::vector<char> seen(scene.nodes.size(), 0);
        int cur = static_cast<int>(start);
        while (cur >= 0) {
            if (seen[cur]) {
                repaired_parent[cur] = -1;
                diags.push_back({DiagSeverity::Warning, "GLB_HIERARCHY_CYCLE",
                    "Cycle at node '" + scene.nodes[cur].name + "'; link broken for GLB export"});
                break;
            }
            seen[cur] = 1;
            cur = repaired_parent[cur];
        }
    }

    // Authoritative world-transform resolver: recursive, independent of
    // node storage order, over the repaired (acyclic) hierarchy.
    std::vector<Mat4> world_matrices(scene.nodes.size(), Mat4::identity());
    std::vector<uint8_t> world_state(scene.nodes.size(), 0);
    std::function<void(int)> resolve_world = [&](int ni) {
        if (ni < 0 || ni >= static_cast<int>(scene.nodes.size()) || world_state[ni] == 2) return;
        if (world_state[ni] == 1) {
            // Unreachable with repaired_parent (acyclic by construction);
            // kept as a termination safety net.
            world_matrices[ni] = scene.nodes[ni].local_matrix;
            world_state[ni] = 2;
            return;
        }
        world_state[ni] = 1;
        int parent = repaired_parent[ni];
        if (parent >= 0) {
            resolve_world(parent);
            world_matrices[ni] = scene.nodes[ni].local_matrix * world_matrices[parent];
        } else {
            world_matrices[ni] = scene.nodes[ni].local_matrix;
        }
        world_state[ni] = 2;
    };
    for (size_t ni = 0; ni < scene.nodes.size(); ++ni) resolve_world(static_cast<int>(ni));

    // Every node that references a mesh, in node order. Model-space
    // localization is per referencing node, never "first node wins".
    std::vector<std::vector<int>> mesh_nodes(scene.meshes.size());
    for (size_t ni = 0; ni < scene.nodes.size(); ++ni) {
        int mi = scene.nodes[ni].mesh_index;
        if (mi >= 0 && mi < static_cast<int>(scene.meshes.size())) {
            mesh_nodes[mi].push_back(static_cast<int>(ni));
        }
    }
    std::vector<bool> detached_mesh(scene.meshes.size(), false);
    std::vector<bool> mesh_skin_exported(scene.meshes.size(), false);
    std::vector<bool> mesh_shared_model_space(scene.meshes.size(), false);

    // Map Scene::meshes -> glTF meshes (split primitives by material)
    std::vector<int> mesh_map(scene.meshes.size(), -1);
    // Instance-localized copies (model-space meshes referenced by several
    // nodes) reuse the shared uv/color accessors and primitive groups.
    std::vector<int> mesh_shared_uv_accessor(scene.meshes.size(), -1);
    std::vector<int> mesh_shared_col_accessor(scene.meshes.size(), -1);
    std::vector<std::map<int, std::vector<uint32_t>>> mesh_primitive_groups(scene.meshes.size());
    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        const auto& m = scene.meshes[mi];
        if (m.vertices.empty() || m.indices.empty()) continue;

        // Model-space coordinates must not be transformed twice. Convert
        // them into the referencing node's local space via its inverse world
        // matrix. Non-invertible transforms get an identity node instead.
        Mat4 localize = Mat4::identity();
        bool localize_ok = true;
        if (m.vertices_in_model_space) {
            const auto& nodes_ref = mesh_nodes[mi];
            // Skinned geometry lives in bind/model space together with the
            // inverse bind matrices. glTF skinning evaluates joints in that
            // space and must not also receive a mesh-node localization.
            if (m.is_skinned) {
                localize_ok = false;
                detached_mesh[mi] = true;
                diags.push_back({DiagSeverity::Info, "GLB_SKINNED_MODEL_SPACE",
                    "Skinned mesh '" + m.name + "' keeps model-space bind coordinates under an identity node"});
            } else if (nodes_ref.size() == 1 && world_matrices[nodes_ref[0]].invert_affine(localize)) {
                // ok — exactly one referencing node, invertible transform
            } else if (nodes_ref.size() == 1) {
                localize_ok = false;
                detached_mesh[mi] = true;
                diags.push_back({DiagSeverity::Warning, "GLB_MODEL_SPACE_FALLBACK",
                    "Mesh '" + m.name + "' has model-space geometry with a non-invertible node transform; placed under an identity node"});
            } else if (nodes_ref.empty()) {
                localize_ok = false;
                detached_mesh[mi] = true;
                diags.push_back({DiagSeverity::Warning, "GLB_MODEL_SPACE_UNREFERENCED",
                    "Model-space mesh '" + m.name + "' has no referencing node; exported under an identity node"});
            } else {
                // Multiple referencing nodes: one localized buffer cannot
                // be correct under two different transforms. Emit one glTF
                // mesh per instance so each instance is exact.
                mesh_shared_model_space[mi] = true;
                localize_ok = false;
                detached_mesh[mi] = false;
                diags.push_back({DiagSeverity::Warning, "GLB_MODEL_SPACE_INSTANCED",
                    "Model-space mesh '" + m.name + "' is referenced by " +
                    std::to_string(nodes_ref.size()) + " nodes; exporting per-instance local copies"});
            }
        }

        size_t nv = m.vertices.size();
        std::vector<float> pos(nv * 3), norm(nv * 3), uv(nv * 2), col(nv * 4);
        std::vector<float> vmin = {1e30f, 1e30f, 1e30f}, vmax = {-1e30f, -1e30f, -1e30f};
        // Normal contract, derived once: the exporter normalizes the source
        // normal to a WORLD direction, then converts world -> local. For a
        // row-vector source M_row with column-vector glTF node matrix
        // M_col = M_row^T, the world-to-local normal transform is
        // M_col^T = M_row (NOT the inverse used for points): with
        // S = diag(2,1,1), Q = R(+90deg Z), the normalized world normal
        // (1,0,0) maps to local (1,0,0); the inverse matrix maps it to
        // (-1,0,0). This keeps normals invariant under nonuniform scale
        // without sign flips and under det<0 without ad-hoc negation.
        for (size_t i = 0; i < nv; ++i) {
            const auto& v = m.vertices[i];
            Vec3 p = (m.vertices_in_model_space && localize_ok) ? localize.transform_point(v.pos) : v.pos;
            Vec3 n = v.normal;
            if (m.vertices_in_model_space && localize_ok) {
                // World -> local for normals uses the TRANSPOSE of the row
                // world matrix (NOT the matrix itself, NOT the point
                // inverse): glTF evaluates n_world = invT(M_col)*n_local
                // with M_col = W_row^T, so n_local = n_world*W_row^T. With
                // S=(2,1,1), Q=R(+90deg Z), the normalized world normal
                // (1,0,0) reconstructs as (1,0,0); the untransposed product
                // reconstructs as (-1,0,0). Mirrored (det<0) transforms stay
                // correct with no ad-hoc sign flips.
                n = world_matrices[mesh_nodes[mi][0]].transposed().transform_vector(v.normal).normalized();
            }
            pos[i*3+0]=p.x; pos[i*3+1]=p.y; pos[i*3+2]=p.z;
            vmin[0]=std::min(vmin[0],p.x); vmin[1]=std::min(vmin[1],p.y); vmin[2]=std::min(vmin[2],p.z);
            vmax[0]=std::max(vmax[0],p.x); vmax[1]=std::max(vmax[1],p.y); vmax[2]=std::max(vmax[2],p.z);
            norm[i*3+0]=n.x; norm[i*3+1]=n.y; norm[i*3+2]=n.z;
            // glTF UV origin is top-left; canonical scene is bottom-left => flip V
            uv[i*2+0]=v.uv.x; uv[i*2+1]=1.0f - v.uv.y;
            col[i*4+0]=v.color.x; col[i*4+1]=v.color.y; col[i*4+2]=v.color.z; col[i*4+3]=v.color.w;
        }
        int acc_pos = add_accessor(accessors, add_buffer_view(buffer_views, bin, pos.data(), pos.size()*4, 34962), 5126, nv, "VEC3", &vmin, &vmax);
        int acc_norm = add_accessor(accessors, add_buffer_view(buffer_views, bin, norm.data(), norm.size()*4, 34962), 5126, nv, "VEC3");
        int acc_uv = m.has_uvs ? add_accessor(accessors, add_buffer_view(buffer_views, bin, uv.data(), uv.size()*4, 34962), 5126, nv, "VEC2") : -1;
        int acc_col = m.has_vertex_colors ? add_accessor(accessors, add_buffer_view(buffer_views, bin, col.data(), col.size()*4, 34962), 5126, nv, "VEC4") : -1;
        mesh_shared_uv_accessor[mi] = acc_uv;
        mesh_shared_col_accessor[mi] = acc_col;

        std::vector<int> acc_joint_sets, acc_weight_sets;
        bool skin_bind_ok = m.is_skinned && skin_idx >= 0;
        if (skin_bind_ok) {
            // Preflight: a node.skin reference is only legal when every
            // vertex has a complete weight list and every bone index maps
            // to a joint. Anything else is dropped skinning, never a silent
            // skin reference without JOINTS/WEIGHTS attributes.
            if (m.skin_weights.size() != nv) {
                skin_bind_ok = false;
                diags.push_back({DiagSeverity::Error, "GLB_SKIN_DATA_INCOMPLETE",
                    "Skinned mesh '" + m.name + "' has " + std::to_string(m.skin_weights.size()) +
                    " weight lists for " + std::to_string(nv) +
                    " vertices; skinning dropped, geometry exported rigid"});
            } else {
                bool all_valid = true;
                bool all_empty = true;
                for (size_t i = 0; i < nv && all_valid; ++i) {
                    float sum = 0.0f;
                    for (const auto& bw : m.skin_weights[i]) {
                        if (bw.bone_node_index < 0 ||
                            !node_to_joint.count(bw.bone_node_index) ||
                            !std::isfinite(bw.weight)) {
                            all_valid = false;
                            break;
                        }
                        sum += bw.weight;
                        if (bw.weight > 1e-6f) all_empty = false;
                    }
                    if (std::fabs(sum - 1.0f) > 0.05f && !m.skin_weights[i].empty()) all_valid = false;
                }
                if (!all_valid) {
                    skin_bind_ok = false;
                    diags.push_back({DiagSeverity::Error, "GLB_SKIN_DATA_INCOMPLETE",
                        "Skinned mesh '" + m.name + "' has invalid bone indices, weights or normalization; "
                        "skinning dropped, geometry exported rigid"});
                } else if (all_empty) {
                    skin_bind_ok = false;
                    diags.push_back({DiagSeverity::Warning, "GLB_SKIN_WEIGHTS_EMPTY",
                        "Skinned mesh '" + m.name + "' has only zero weights; skinning dropped, geometry exported rigid"});
                }
            }
        }
        if (skin_bind_ok) {
            mesh_skin_exported[mi] = true;
            const size_t limit = opts.glb_max_influences;
            // Select once per vertex with the exact configured limit. The
            // resulting list is already deduplicated, sorted, capped and
            // normalized over the kept influences only.
            std::vector<std::vector<SkinInfluence>> per_vertex(nv);
            size_t max_exported = 0;
            bool lost = false;
            for (size_t i = 0; i < nv; ++i) {
                auto full = select_skin_influences(m.skin_weights[i], node_to_joint, 0);
                per_vertex[i] = select_skin_influences(m.skin_weights[i], node_to_joint, limit);
                if (limit > 0 && per_vertex[i].size() < full.size()) lost = true;
                max_exported = std::max(max_exported, per_vertex[i].size());
            }
            if (lost) {
                diags.push_back({DiagSeverity::Error, "GLB_INFLUENCE_LIMIT_LOSS",
                    "Mesh '" + m.name + "' has influences exceeding compatibility profile limit " +
                    std::to_string(limit) + " - influences truncated explicitly"});
            }
            const size_t sets = std::max<size_t>(1, (max_exported + 3) / 4);
            for (size_t set_i = 0; set_i < sets; ++set_i) {
                std::vector<uint16_t> jbuf(nv * 4, 0);
                std::vector<float> wbuf(nv * 4, 0.0f);
                for (size_t i = 0; i < nv; ++i) {
                    const auto& inf = per_vertex[i];
                    for (size_t k = 0; k < inf.size(); ++k) {
                        size_t target_set = k / 4, slot = k % 4;
                        if (target_set == set_i) {
                            jbuf[i*4 + slot] = inf[k].joint;
                            wbuf[i*4 + slot] = inf[k].weight;
                        }
                    }
                }
                acc_joint_sets.push_back(add_accessor(accessors, add_buffer_view(buffer_views, bin, jbuf.data(), jbuf.size()*2, 34962), 5123, nv, "VEC4"));
                acc_weight_sets.push_back(add_accessor(accessors, add_buffer_view(buffer_views, bin, wbuf.data(), wbuf.size()*4, 34962), 5126, nv, "VEC4"));
            }
        }

        // Group triangles by material
        std::map<int, std::vector<uint32_t>>& by_mat = mesh_primitive_groups[mi];
        size_t ntris = m.indices.size() / 3;
        for (size_t t = 0; t < ntris; ++t) {
            int mat_id = (opts.include_materials && t < m.triangle_materials.size()) ? m.triangle_materials[t] : -1;
            auto& bucket = by_mat[mat_id];
            bucket.push_back(m.indices[t*3+0]);
            bucket.push_back(m.indices[t*3+1]);
            bucket.push_back(m.indices[t*3+2]);
        }

        Json prims = Json::array();
        for (const auto& [mat_id, idxs] : by_mat) {
            if (idxs.empty()) continue;
            int acc_idx = add_accessor(accessors, add_buffer_view(buffer_views, bin, idxs.data(), idxs.size()*4, 34963), 5125, idxs.size(), "SCALAR");
            Json attrs = Json::object();
            attrs["POSITION"] = acc_pos;
            attrs["NORMAL"] = acc_norm;
            if (acc_uv >= 0) attrs["TEXCOORD_0"] = acc_uv;
            if (acc_col >= 0) attrs["COLOR_0"] = acc_col;
            for (size_t si = 0; si < acc_joint_sets.size(); ++si) {
                attrs["JOINTS_" + std::to_string(si)] = acc_joint_sets[si];
                attrs["WEIGHTS_" + std::to_string(si)] = acc_weight_sets[si];
            }
            Json prim = Json::object();
            prim["attributes"] = std::move(attrs);
            prim["indices"] = acc_idx;
            if (mat_id >= 0 && mat_id < static_cast<int>(materials.as_arr().size())) prim["material"] = mat_id;
            prims.push_back(std::move(prim));
        }

        Json jm = Json::object();
        jm["name"] = m.name.empty() ? ("mesh_" + std::to_string(mi)) : m.name;
        jm["primitives"] = std::move(prims);
        mesh_map[mi] = static_cast<int>(meshes.as_arr().size());
        meshes.push_back(std::move(jm));
    }

    // Per-instance copies for shared model-space meshes: one glTF mesh per
    // referencing node, each localized to that node's own inverse world
    // matrix and bound back to that same node (which keeps its TRS).
    std::unordered_map<int, int> instance_mesh_of_node;
    for (size_t mi = 0; mi < mesh_shared_model_space.size(); ++mi) {
        if (!mesh_shared_model_space[mi]) continue;
        const auto& m = scene.meshes[mi];
        for (int ni : mesh_nodes[mi]) {
            Mat4 loc = Mat4::identity();
            if (!world_matrices[ni].invert_affine(loc)) {
                diags.push_back({DiagSeverity::Warning, "GLB_MODEL_SPACE_FALLBACK",
                    "Instance node '" + scene.nodes[ni].name + "' of mesh '" + m.name +
                    "' has a non-invertible transform; instance exported under identity"});
            }
            size_t nv = m.vertices.size();
            std::vector<float> pos(nv * 3), norm(nv * 3);
            std::vector<float> vmin = {1e30f, 1e30f, 1e30f}, vmax = {-1e30f, -1e30f, -1e30f};
            for (size_t i = 0; i < nv; ++i) {
                const auto& v = m.vertices[i];
                Vec3 p = loc.transform_point(v.pos);
                // Same world->local normal contract as the single-node path.
                Vec3 n = world_matrices[ni].transposed().transform_vector(v.normal).normalized();
                pos[i*3+0]=p.x; pos[i*3+1]=p.y; pos[i*3+2]=p.z;
                vmin[0]=std::min(vmin[0],p.x); vmin[1]=std::min(vmin[1],p.y); vmin[2]=std::min(vmin[2],p.z);
                vmax[0]=std::max(vmax[0],p.x); vmax[1]=std::max(vmax[1],p.y); vmax[2]=std::max(vmax[2],p.z);
                norm[i*3+0]=n.x; norm[i*3+1]=n.y; norm[i*3+2]=n.z;
            }
            int acc_pos = add_accessor(accessors, add_buffer_view(buffer_views, bin, pos.data(), pos.size()*4, 34962), 5126, nv, "VEC3", &vmin, &vmax);
            int acc_norm = add_accessor(accessors, add_buffer_view(buffer_views, bin, norm.data(), norm.size()*4, 34962), 5126, nv, "VEC3");

            Json prims = Json::array();
            for (const auto& [mat_id, idxs] : mesh_primitive_groups[mi]) {
                if (idxs.empty()) continue;
                int acc_idx = add_accessor(accessors, add_buffer_view(buffer_views, bin, idxs.data(), idxs.size()*4, 34963), 5125, idxs.size(), "SCALAR");
                Json attrs = Json::object();
                attrs["POSITION"] = acc_pos;
                attrs["NORMAL"] = acc_norm;
                if (mesh_shared_uv_accessor[mi] >= 0) attrs["TEXCOORD_0"] = mesh_shared_uv_accessor[mi];
                if (mesh_shared_col_accessor[mi] >= 0) attrs["COLOR_0"] = mesh_shared_col_accessor[mi];
                Json prim = Json::object();
                prim["attributes"] = std::move(attrs);
                prim["indices"] = acc_idx;
                if (mat_id >= 0 && mat_id < static_cast<int>(materials.as_arr().size())) prim["material"] = mat_id;
                prims.push_back(std::move(prim));
            }
            Json jm = Json::object();
            jm["name"] = scene.meshes[mi].name + "_" + scene.nodes[ni].name;
            jm["primitives"] = std::move(prims);
            int inst_mesh_idx = static_cast<int>(meshes.as_arr().size());
            meshes.push_back(std::move(jm));

            // The instance mesh is bound to the ORIGINAL referencing node:
            // it already carries the right TRS, and the geometry above was
            // localized to that node, so world placement is exact with no
            // double transform.
            instance_mesh_of_node[ni] = inst_mesh_idx;
        }
    }

    // Nodes (use TRS so glTF animations can target translation/rotation/scale legally!)
    // children is derived from the repaired parent links, so the serialized
    // hierarchy is acyclic by construction, not just the matrices.
    for (size_t ni = 0; ni < scene.nodes.size(); ++ni) {
        const auto& n = scene.nodes[ni];
        Json jn = Json::object();
        jn["name"] = n.name;
        Vec3 t, s; Quat q;
        n.local_matrix.decompose_column_trs(t, q, s);
        Json jt = Json::array(); jt.push_back(t.x); jt.push_back(t.y); jt.push_back(t.z);
        Json jr = Json::array(); jr.push_back(q.x); jr.push_back(q.y); jr.push_back(q.z); jr.push_back(q.w);
        Json js = Json::array(); js.push_back(s.x); js.push_back(s.y); js.push_back(s.z);
        jn["translation"] = std::move(jt);
        jn["rotation"] = std::move(jr);
        jn["scale"] = std::move(js);

            if (n.mesh_index >= 0 && n.mesh_index < static_cast<int>(mesh_map.size()) &&
            mesh_map[n.mesh_index] >= 0 && !detached_mesh[n.mesh_index] &&
            !mesh_shared_model_space[n.mesh_index]) {
            jn["mesh"] = mesh_map[n.mesh_index];
            if (mesh_skin_exported[n.mesh_index]) jn["skin"] = skin_idx;
        }
        auto inst_it = instance_mesh_of_node.find(static_cast<int>(ni));
        if (inst_it != instance_mesh_of_node.end()) {
            jn["mesh"] = inst_it->second;
            if (mesh_skin_exported[n.mesh_index]) jn["skin"] = skin_idx;
        }
        Json ch = Json::array();
        for (size_t ci = 0; ci < scene.nodes.size(); ++ci) {
            if (repaired_parent[ci] == static_cast<int>(ni)) ch.push_back(static_cast<int>(ci));
        }
        if (!ch.as_arr().empty()) jn["children"] = std::move(ch);
        nodes.push_back(std::move(jn));
    }

    // Identity nodes for model-space meshes with non-invertible source
    // transforms. Appended after all indexed nodes so xrefs stay stable.
    std::vector<int> extra_roots;
    for (size_t mi = 0; mi < detached_mesh.size(); ++mi) {
        if (!detached_mesh[mi] || mesh_map[mi] < 0) continue;
        Json jn = Json::object();
        jn["name"] = scene.meshes[mi].name + "_model_space";
        jn["mesh"] = mesh_map[mi];
        if (mesh_skin_exported[mi]) jn["skin"] = skin_idx;
        extra_roots.push_back(static_cast<int>(nodes.as_arr().size()));
        nodes.push_back(std::move(jn));
    }

    // Animations
    if (opts.include_animations) {
        for (const auto& clip : scene.animations) {
            Json jch = Json::array(), jsmp = Json::array();
            float fps = (clip.fps > 0.0f) ? clip.fps : 20.0f;
            float base_frame = clip.start_frame;

            auto add_vec3_track = [&](int node_idx, const std::string& path, const std::vector<KeyframeVec3>& keys) {
                if (keys.empty()) return;
                std::vector<float> times, vals;
                times.reserve(keys.size()); vals.reserve(keys.size() * 3);
                float tmin = 1e30f, tmax = -1e30f;
                for (const auto& k : keys) {
                    float t = std::max(0.0f, (k.frame - base_frame) / fps);
                    tmin = std::min(tmin, t); tmax = std::max(tmax, t);
                    times.push_back(t);
                    vals.push_back(k.value.x); vals.push_back(k.value.y); vals.push_back(k.value.z);
                }
                std::vector<float> mn = {tmin}, mx = {tmax};
                int acc_t = add_accessor(accessors, add_buffer_view(buffer_views, bin, times.data(), times.size()*4), 5126, times.size(), "SCALAR", &mn, &mx);
                int acc_v = add_accessor(accessors, add_buffer_view(buffer_views, bin, vals.data(), vals.size()*4), 5126, keys.size(), "VEC3");
                int si = static_cast<int>(jsmp.as_arr().size());
                Json sm = Json::object(); sm["input"] = acc_t; sm["output"] = acc_v; sm["interpolation"] = "LINEAR";
                jsmp.push_back(std::move(sm));
                Json ch = Json::object(); ch["sampler"] = si;
                Json tgt = Json::object(); tgt["node"] = node_idx; tgt["path"] = path;
                ch["target"] = std::move(tgt);
                jch.push_back(std::move(ch));
            };

            auto add_quat_track = [&](int node_idx, const std::vector<KeyframeQuat>& keys) {
                if (keys.empty()) return;
                std::vector<float> times, vals;
                times.reserve(keys.size()); vals.reserve(keys.size() * 4);
                float tmin = 1e30f, tmax = -1e30f;
                for (const auto& k : keys) {
                    float t = std::max(0.0f, (k.frame - base_frame) / fps);
                    tmin = std::min(tmin, t); tmax = std::max(tmax, t);
                    times.push_back(t);
                    Quat q = k.value.normalized();
                    vals.push_back(q.x); vals.push_back(q.y); vals.push_back(q.z); vals.push_back(q.w);
                }
                std::vector<float> mn = {tmin}, mx = {tmax};
                int acc_t = add_accessor(accessors, add_buffer_view(buffer_views, bin, times.data(), times.size()*4), 5126, times.size(), "SCALAR", &mn, &mx);
                int acc_v = add_accessor(accessors, add_buffer_view(buffer_views, bin, vals.data(), vals.size()*4), 5126, keys.size(), "VEC4");
                int si = static_cast<int>(jsmp.as_arr().size());
                Json sm = Json::object(); sm["input"] = acc_t; sm["output"] = acc_v; sm["interpolation"] = "LINEAR";
                jsmp.push_back(std::move(sm));
                Json ch = Json::object(); ch["sampler"] = si;
                Json tgt = Json::object(); tgt["node"] = node_idx; tgt["path"] = "rotation";
                ch["target"] = std::move(tgt);
                jch.push_back(std::move(ch));
            };

            for (const auto& tr : clip.tracks) {
                if (tr.node_index < 0 || tr.node_index >= static_cast<int>(scene.nodes.size())) continue;
                add_vec3_track(tr.node_index, "translation", tr.translations);
                add_quat_track(tr.node_index, tr.rotations);
                add_vec3_track(tr.node_index, "scale", tr.scales);
            }
            if (!jch.as_arr().empty()) {
                Json ja = Json::object();
                ja["name"] = clip.name;
                ja["channels"] = std::move(jch);
                ja["samplers"] = std::move(jsmp);
                animations.push_back(std::move(ja));
            }
        }
    }

    Json buffers = Json::array();
    {
        Json b = Json::object();
        b["byteLength"] = bin.size();
        if (!binary_glb) {
            auto bin_path = out_path;
            bin_path.replace_extension(L".bin");
            b["uri"] = utf8_from_path(bin_path.filename());
            bundle.add(bin_path, bin, "glTF binary buffer");
        }
        buffers.push_back(std::move(b));
    }

    Json jscene = Json::object();
    jscene["name"] = scene.source_name;
    Json roots = Json::array();
    // A node is a scene root when no repaired parent links to it. For the
    // normal (non-cyclic) tree this is exactly Scene::root_nodes.
    for (size_t ni = 0; ni < scene.nodes.size(); ++ni) {
        if (repaired_parent[ni] < 0) roots.push_back(static_cast<int>(ni));
    }
    for (int r : extra_roots) roots.push_back(r);
    jscene["nodes"] = std::move(roots);
    Json jscenes = Json::array();
    jscenes.push_back(std::move(jscene));

    gltf["scene"] = 0;
    gltf["scenes"] = std::move(jscenes);
    gltf["nodes"] = std::move(nodes);
    gltf["buffers"] = std::move(buffers);
    if (!buffer_views.as_arr().empty()) gltf["bufferViews"] = std::move(buffer_views);
    if (!accessors.as_arr().empty()) gltf["accessors"] = std::move(accessors);
    if (!meshes.as_arr().empty()) gltf["meshes"] = std::move(meshes);
    if (!materials.as_arr().empty()) gltf["materials"] = std::move(materials);
    if (!images.as_arr().empty()) {
        gltf["images"] = std::move(images);
        gltf["textures"] = std::move(textures);
        gltf["samplers"] = std::move(samplers);
    }
    if (!skins.as_arr().empty()) gltf["skins"] = std::move(skins);
    if (!animations.as_arr().empty()) gltf["animations"] = std::move(animations);

    if (!binary_glb) {
        std::string js = gltf.dump(2);
        bundle.add(out_path, std::vector<uint8_t>(js.begin(), js.end()), "glTF document");
        return bundle.commit(opts.overwrite, cancel_flag, err);
    }

    // Assemble GLB v2 binary container
    std::string js = gltf.dump(-1);
    while (js.size() % 4 != 0) js.push_back(' ');
    align4(bin);

    uint32_t total_len = 12u + 8u + static_cast<uint32_t>(js.size()) + 8u + static_cast<uint32_t>(bin.size());
    std::vector<uint8_t> glb;
    glb.reserve(total_len);
    auto push_u32 = [&](uint32_t v) {
        uint8_t b[4]; std::memcpy(b, &v, 4);
        glb.insert(glb.end(), b, b + 4);
    };
    push_u32(0x46546C67u); // "glTF"
    push_u32(2u);          // version 2
    push_u32(total_len);
    push_u32(static_cast<uint32_t>(js.size()));
    push_u32(0x4E4F534Au); // "JSON"
    glb.insert(glb.end(), js.begin(), js.end());
    push_u32(static_cast<uint32_t>(bin.size()));
    push_u32(0x004E4942u); // "BIN\0"
    glb.insert(glb.end(), bin.begin(), bin.end());

    bundle.add(out_path, std::move(glb), "GLB document");
    return bundle.commit(opts.overwrite, cancel_flag, err);
}

} // namespace bz
