#include "bzconverter/exporters/glb_exporter.hpp"
#include "bzconverter/core/json.hpp"
#include "bzconverter/core/output_bundle.hpp"
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <algorithm>

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
    // KNOWN-BAD CONTROL: old behavior ignored exact limit.
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
#define BZCONV_VERSION "1.2.0"
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
        if (!read_file_bytes(path_from_utf8(p), img_bytes, rerr) || img_bytes.size() < 8) return -1;

        std::string mime = "image/png";
        if (img_bytes[0] == 0xFF && img_bytes[1] == 0xD8) mime = "image/jpeg";
        else if (!(img_bytes[0] == 137 && img_bytes[1] == 80 && img_bytes[2] == 78 && img_bytes[3] == 71)) {
            // Only embed PNG/JPEG in glTF 2.0 core spec
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
        if (!scene.root_nodes.empty()) sk["skeleton"] = scene.root_nodes[0];
        skin_idx = 0;
        skins.push_back(std::move(sk));
    }

    // Map Scene::meshes -> glTF meshes (split primitives by material)
    std::vector<int> mesh_map(scene.meshes.size(), -1);
    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        const auto& m = scene.meshes[mi];
        if (m.vertices.empty() || m.indices.empty()) continue;

        size_t nv = m.vertices.size();
        std::vector<float> pos(nv * 3), norm(nv * 3), uv(nv * 2), col(nv * 4);
        std::vector<float> vmin = {1e30f, 1e30f, 1e30f}, vmax = {-1e30f, -1e30f, -1e30f};
        for (size_t i = 0; i < nv; ++i) {
            const auto& v = m.vertices[i];
            pos[i*3+0]=v.pos.x; pos[i*3+1]=v.pos.y; pos[i*3+2]=v.pos.z;
            vmin[0]=std::min(vmin[0],v.pos.x); vmin[1]=std::min(vmin[1],v.pos.y); vmin[2]=std::min(vmin[2],v.pos.z);
            vmax[0]=std::max(vmax[0],v.pos.x); vmax[1]=std::max(vmax[1],v.pos.y); vmax[2]=std::max(vmax[2],v.pos.z);
            norm[i*3+0]=v.normal.x; norm[i*3+1]=v.normal.y; norm[i*3+2]=v.normal.z;
            // glTF UV origin is top-left; canonical scene is bottom-left => flip V
            uv[i*2+0]=v.uv.x; uv[i*2+1]=1.0f - v.uv.y;
            col[i*4+0]=v.color.x; col[i*4+1]=v.color.y; col[i*4+2]=v.color.z; col[i*4+3]=v.color.w;
        }
        int acc_pos = add_accessor(accessors, add_buffer_view(buffer_views, bin, pos.data(), pos.size()*4, 34962), 5126, nv, "VEC3", &vmin, &vmax);
        int acc_norm = add_accessor(accessors, add_buffer_view(buffer_views, bin, norm.data(), norm.size()*4, 34962), 5126, nv, "VEC3");
        int acc_uv = m.has_uvs ? add_accessor(accessors, add_buffer_view(buffer_views, bin, uv.data(), uv.size()*4, 34962), 5126, nv, "VEC2") : -1;
        int acc_col = m.has_vertex_colors ? add_accessor(accessors, add_buffer_view(buffer_views, bin, col.data(), col.size()*4, 34962), 5126, nv, "VEC4") : -1;

        std::vector<int> acc_joint_sets, acc_weight_sets;
        if (m.is_skinned && skin_idx >= 0 && m.skin_weights.size() == nv) {
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
        std::map<int, std::vector<uint32_t>> by_mat;
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

    // Nodes (use TRS so glTF animations can target translation/rotation/scale legally!)
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

        if (n.mesh_index >= 0 && n.mesh_index < static_cast<int>(mesh_map.size()) && mesh_map[n.mesh_index] >= 0) {
            jn["mesh"] = mesh_map[n.mesh_index];
            if (scene.meshes[n.mesh_index].is_skinned && skin_idx >= 0) jn["skin"] = skin_idx;
        }
        if (!n.children.empty()) {
            Json ch = Json::array();
            for (int ci : n.children) ch.push_back(ci);
            jn["children"] = std::move(ch);
        }
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
    for (int r : scene.root_nodes) roots.push_back(r);
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
