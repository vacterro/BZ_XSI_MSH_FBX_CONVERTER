#include "bzconverter/exporters/fbx_exporter.hpp"
#include "bzconverter/core/json.hpp"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

namespace bz {

namespace {

std::wstring quote_arg(const std::wstring& a) {
    std::wstring out = L"\"";
    for (wchar_t c : a) {
        if (c == L'"') out += L"\\\"";
        else out.push_back(c);
    }
    out.push_back(L'"');
    return out;
}

bool run_process_capture(const std::wstring& cmdline, std::string& stdout_out, DWORD& exit_code,
                         std::atomic<bool>* cancel_flag, int timeout_ms = 180000) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmdline;
    BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);
    if (!ok) {
        CloseHandle(hRead);
        return false;
    }

    stdout_out.clear();
    auto t0 = std::chrono::steady_clock::now();
    char buf[4096];
    bool cancelled = false;

    while (true) {
        DWORD avail = 0;
        while (PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD got = 0;
            if (ReadFile(hRead, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) && got > 0) {
                if (stdout_out.size() < 256 * 1024) stdout_out.append(buf, got);
            } else break;
        }
        if (cancel_flag && cancel_flag->load()) {
            TerminateProcess(pi.hProcess, 130);
            cancelled = true;
            break;
        }
        DWORD wait = WaitForSingleObject(pi.hProcess, 40);
        if (wait == WAIT_OBJECT_0) break;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsed > timeout_ms) {
            TerminateProcess(pi.hProcess, 124);
            break;
        }
    }

    // Drain remaining output
    DWORD avail = 0;
    while (PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        DWORD got = 0;
        if (ReadFile(hRead, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) && got > 0) {
            if (stdout_out.size() < 256 * 1024) stdout_out.append(buf, got);
        } else break;
    }

    exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    return !cancelled;
}

std::filesystem::path get_exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0) return std::filesystem::path(buf).parent_path();
    return std::filesystem::current_path();
}

void write_canonical_payload(const Scene& scene, const ExportOptions& opts,
                             const std::filesystem::path& json_path,
                             const std::filesystem::path& bin_path,
                             std::string& err) {
    std::vector<uint8_t> bin;
    auto push_bytes = [&](const void* ptr, size_t len) -> size_t {
        while (bin.size() % 4 != 0) bin.push_back(0);
        size_t off = bin.size();
        if (len > 0) {
            const auto* p = reinterpret_cast<const uint8_t*>(ptr);
            bin.insert(bin.end(), p, p + len);
        }
        return off;
    };

    Json root = Json::object();
    root["sourceName"] = scene.source_name;
    root["sourceFormat"] = format_to_string(scene.source_format);

    Json jopts = Json::object();
    jopts["includeMaterials"] = opts.include_materials;
    jopts["includeTextures"] = opts.include_textures;
    jopts["includeAnimations"] = opts.include_animations;
    jopts["preserveHierarchy"] = opts.preserve_hierarchy;
    jopts["embedTextures"] = opts.embed_textures;
    jopts["bzccAxisMode"] = opts.bzcc_axis_mode;
    root["options"] = std::move(jopts);

    Json jmats = Json::array();
    for (const auto& m : scene.materials) {
        Json jm = Json::object();
        jm["name"] = m.name;
        jm["materialFileRef"] = m.material_file_ref;
        jm["doubleSided"] = m.double_sided;
        jm["hasAlpha"] = m.has_alpha;
        jm["emissiveFlag"] = m.emissive_flag;
        jm["emissiveStrength"] = m.emissive_strength;
        jm["specularPower"] = m.specular_power;
        auto jv4 = [](const Vec4& v) {
            Json a = Json::array(); a.push_back(v.x); a.push_back(v.y); a.push_back(v.z); a.push_back(v.w); return a;
        };
        jm["diffuse"] = jv4(m.diffuse);
        jm["specular"] = jv4(m.specular);
        jm["emissive"] = jv4(m.emissive);
        jm["ambient"] = jv4(m.ambient);
        auto best_tex = [](const TextureChannel& ch) {
            return !ch.converted_png_path.empty() ? ch.converted_png_path : ch.resolved_path;
        };
        jm["diffuseTex"] = best_tex(m.diffuse_tex);
        jm["emissiveTex"] = best_tex(m.emissive_tex);
        jm["normalTex"] = best_tex(m.normal_tex);
        jm["specularTex"] = best_tex(m.specular_tex);
        jm["teamColorTex"] = best_tex(m.team_color_tex);
        jmats.push_back(std::move(jm));
    }
    root["materials"] = std::move(jmats);

    Json jmeshes = Json::array();
    for (const auto& m : scene.meshes) {
        size_t nv = m.vertices.size();
        size_t ni = m.indices.size();
        // FBX/Blender has no 4-influence limit. Serialize all canonical influences as
        // variable-length rows with per-vertex offsets/counts; never truncate silently.
        std::vector<float> pos(nv * 3), norm(nv * 3), uv(nv * 2), col(nv * 4);
        std::vector<int32_t> joint_data;
        std::vector<float> weight_data;
        std::vector<uint32_t> inf_counts(nv, 0), inf_offsets(nv, 0);
        size_t max_influences = 0;
        for (size_t i = 0; i < nv; ++i) {
            pos[i*3]=m.vertices[i].pos.x; pos[i*3+1]=m.vertices[i].pos.y; pos[i*3+2]=m.vertices[i].pos.z;
            norm[i*3]=m.vertices[i].normal.x; norm[i*3+1]=m.vertices[i].normal.y; norm[i*3+2]=m.vertices[i].normal.z;
            uv[i*2]=m.vertices[i].uv.x; uv[i*2+1]=m.vertices[i].uv.y;
            col[i*4]=m.vertices[i].color.x; col[i*4+1]=m.vertices[i].color.y; col[i*4+2]=m.vertices[i].color.z; col[i*4+3]=m.vertices[i].color.w;
            if (m.is_skinned && i < m.skin_weights.size()) {
                inf_offsets[i] = static_cast<uint32_t>(joint_data.size());
                for (const auto& bw : m.skin_weights[i]) {
                    if (bw.bone_node_index < 0 || bw.bone_node_index >= static_cast<int>(scene.nodes.size())) continue;
                    if (!(bw.weight > 1e-6f) || !std::isfinite(bw.weight)) continue;
                    joint_data.push_back(bw.bone_node_index);
                    weight_data.push_back(bw.weight);
                }
                inf_counts[i] = static_cast<uint32_t>(weight_data.size() - inf_offsets[i]);
                max_influences = std::max(max_influences, static_cast<size_t>(inf_counts[i]));
            }
        }
        std::vector<int32_t> tmats(m.triangle_materials.begin(), m.triangle_materials.end());
        Json jm = Json::object();
        jm["name"] = m.name;
        jm["vertexCount"] = nv;
        jm["indexCount"] = ni;
        jm["hasNormals"] = m.has_normals;
        jm["hasUvs"] = m.has_uvs;
        jm["hasVertexColors"] = m.has_vertex_colors;
        jm["isSkinned"] = m.is_skinned;
        jm["verticesInModelSpace"] = m.vertices_in_model_space;
        jm["maxInfluences"] = max_influences;
        jm["jointOffset"] = push_bytes(joint_data.data(), joint_data.size() * 4);
        jm["weightOffset"] = push_bytes(weight_data.data(), weight_data.size() * 4);
        jm["influenceCountOffset"] = push_bytes(inf_counts.data(), inf_counts.size() * 4);
        jm["influenceOffsetOffset"] = push_bytes(inf_offsets.data(), inf_offsets.size() * 4);
        jm["posOffset"] = push_bytes(pos.data(), pos.size() * 4);
        jm["normOffset"] = push_bytes(norm.data(), norm.size() * 4);
        jm["uvOffset"] = push_bytes(uv.data(), uv.size() * 4);
        jm["colOffset"] = push_bytes(col.data(), col.size() * 4);
        jm["idxOffset"] = push_bytes(m.indices.data(), m.indices.size() * 4);
        jm["triMatOffset"] = push_bytes(tmats.data(), tmats.size() * 4);
        jmeshes.push_back(std::move(jm));
    }
    root["meshes"] = std::move(jmeshes);

    auto jmat4 = [](const Mat4& mt) {
        Json a = Json::array();
        for (float f : mt.m) a.push_back(f);
        return a;
    };
    Json jnodes = Json::array();
    for (const auto& n : scene.nodes) {
        Json jn = Json::object();
        jn["name"] = n.name;
        jn["parent"] = n.parent_index;
        jn["meshIndex"] = n.mesh_index;
        jn["isBone"] = n.is_bone;
        jn["isHardpoint"] = n.is_hardpoint;
        jn["localMatrix"] = jmat4(n.local_matrix);
        jn["hasBasePose"] = n.has_base_pose;
        if (n.has_base_pose) jn["basePoseMatrix"] = jmat4(n.base_pose_matrix);
        jn["hasInverseBind"] = n.has_inverse_bind;
        if (n.has_inverse_bind) jn["inverseBindMatrix"] = jmat4(n.inverse_bind_matrix);
        Json ch = Json::array();
        for (int c : n.children) ch.push_back(c);
        jn["children"] = std::move(ch);
        jnodes.push_back(std::move(jn));
    }
    root["nodes"] = std::move(jnodes);

    Json janims = Json::array();
    for (const auto& clip : scene.animations) {
        Json ja = Json::object();
        ja["name"] = clip.name;
        ja["fps"] = clip.fps;
        ja["startFrame"] = clip.start_frame;
        ja["endFrame"] = clip.end_frame;
        Json jtracks = Json::array();
        for (const auto& tr : clip.tracks) {
            Json jt = Json::object();
            jt["nodeIndex"] = tr.node_index;
            Json jtr = Json::array(), jrot = Json::array(), jsc = Json::array();
            for (const auto& k : tr.translations) {
                Json pair = Json::array(); pair.push_back(k.frame);
                Json v = Json::array(); v.push_back(k.value.x); v.push_back(k.value.y); v.push_back(k.value.z);
                pair.push_back(std::move(v)); jtr.push_back(std::move(pair));
            }
            for (const auto& k : tr.rotations) {
                Json pair = Json::array(); pair.push_back(k.frame);
                Json q = Json::array(); q.push_back(k.value.w); q.push_back(k.value.x); q.push_back(k.value.y); q.push_back(k.value.z);
                pair.push_back(std::move(q)); jrot.push_back(std::move(pair));
            }
            for (const auto& k : tr.scales) {
                Json pair = Json::array(); pair.push_back(k.frame);
                Json v = Json::array(); v.push_back(k.value.x); v.push_back(k.value.y); v.push_back(k.value.z);
                pair.push_back(std::move(v)); jsc.push_back(std::move(pair));
            }
            jt["translations"] = std::move(jtr);
            jt["rotations"] = std::move(jrot);
            jt["scales"] = std::move(jsc);
            jtracks.push_back(std::move(jt));
        }
        ja["tracks"] = std::move(jtracks);
        janims.push_back(std::move(ja));
    }
    root["animations"] = std::move(janims);

    std::string js = root.dump(-1);
    write_file_atomic(json_path, js.data(), js.size(), err);
    if (err.empty()) write_file_atomic(bin_path, bin.data(), bin.size(), err);
}

} // namespace

BlenderInfo detect_blender(const std::filesystem::path& explicit_override) {
    BlenderInfo info;
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    if (!explicit_override.empty()) candidates.push_back(explicit_override);

    // Search Blender Foundation directories (prefer LTS 4.5, then newest)
    const wchar_t* roots[] = {
        L"C:\\Program Files\\Blender Foundation",
        L"C:\\Program Files (x86)\\Blender Foundation",
        L"V:\\__STORE_V\\_SOFT\\Steam\\steamapps\\common\\Blender"
    };
    std::vector<std::filesystem::path> found_dirs;
    for (const wchar_t* r : roots) {
        std::filesystem::path rp(r);
        if (!std::filesystem::exists(rp, ec)) continue;
        if (std::filesystem::exists(rp / L"blender.exe", ec)) found_dirs.push_back(rp / L"blender.exe");
        for (const auto& e : std::filesystem::directory_iterator(rp, ec)) {
            if (e.is_directory(ec)) {
                auto exe = e.path() / L"blender.exe";
                if (std::filesystem::exists(exe, ec)) found_dirs.push_back(exe);
            }
        }
    }
    // Prefer 4.5 LTS first (extremely stable FBX IO), then 5.0/other versions descending
    std::sort(found_dirs.begin(), found_dirs.end(), [](const auto& a, const auto& b) {
        auto sa = utf8_from_path(a), sb = utf8_from_path(b);
        bool a45 = sa.find("4.5") != std::string::npos;
        bool b45 = sb.find("4.5") != std::string::npos;
        if (a45 != b45) return a45;
        return sa > sb;
    });
    candidates.insert(candidates.end(), found_dirs.begin(), found_dirs.end());

    // Check PATH
    wchar_t found_path[MAX_PATH * 2];
    if (SearchPathW(nullptr, L"blender.exe", nullptr, static_cast<DWORD>(std::size(found_path)), found_path, nullptr) > 0) {
        candidates.emplace_back(found_path);
    }

    for (const auto& c : candidates) {
        if (!std::filesystem::exists(c, ec) || !std::filesystem::is_regular_file(c, ec)) continue;
        std::string out;
        DWORD rc = 1;
        std::wstring cmd = quote_arg(c.wstring()) + L" --version";
        if (run_process_capture(cmd, out, rc, nullptr, 15000) && rc == 0) {
            info.found = true;
            info.exe_path = c;
            auto nl = out.find('\n');
            info.version_string = trim(nl == std::string::npos ? out : out.substr(0, nl));
            info.diagnostic = "Detected " + info.version_string + " at " + utf8_from_path(c);
            return info;
        }
    }
    info.diagnostic = "Blender executable not found. Install Blender 4.x/5.x or set Blender path in Settings to enable FBX export (GLB/GLTF/OBJ work natively without Blender).";
    return info;
}

std::filesystem::path locate_bridge_script(const std::filesystem::path& explicit_override) {
    std::error_code ec;
    if (!explicit_override.empty() && std::filesystem::exists(explicit_override, ec)) return explicit_override;
    auto exe_dir = get_exe_dir();
    std::vector<std::filesystem::path> cands = {
        exe_dir / L"bz_fbx_bridge.py",
        exe_dir / L"scripts" / L"bz_fbx_bridge.py",
        exe_dir.parent_path() / L"src" / L"bzconverter" / L"exporters" / L"blender_scripts" / L"bz_fbx_bridge.py",
        std::filesystem::current_path(ec) / L"src" / L"bzconverter" / L"exporters" / L"blender_scripts" / L"bz_fbx_bridge.py",
        std::filesystem::current_path(ec) / L"bz_fbx_bridge.py",
    };
    for (const auto& c : cands) {
        if (std::filesystem::exists(c, ec) && std::filesystem::is_regular_file(c, ec)) return c;
    }
    return {};
}

bool export_scene_fbx(const Scene& scene, const std::filesystem::path& out_fbx_path,
                      const ExportOptions& opts, ValidationMetrics& out_val,
                      std::vector<Diagnostic>& diags, std::string& err,
                      std::atomic<bool>* cancel_flag) {
    auto binfo = detect_blender(opts.blender_exe);
    if (!binfo.found) {
        err = binfo.diagnostic;
        diags.push_back({DiagSeverity::Error, "BLENDER_NOT_FOUND", err});
        return false;
    }
    auto script = locate_bridge_script(opts.bridge_script);
    if (script.empty()) {
        err = "Blender FBX bridge script (bz_fbx_bridge.py) not found next to executable or in src/";
        diags.push_back({DiagSeverity::Error, "BRIDGE_SCRIPT_MISSING", err});
        return false;
    }

    std::error_code ec;
    auto uid = std::chrono::steady_clock::now().time_since_epoch().count();
    auto work_dir = std::filesystem::temp_directory_path(ec) / (L"bzconv_fbx_" + std::to_wstring(uid) + L"_" + std::to_wstring(GetCurrentThreadId()));
    std::filesystem::create_directories(work_dir, ec);

    auto json_path = work_dir / L"scene.json";
    auto bin_path = work_dir / L"scene.bin";
    auto tmp_fbx = work_dir / L"out.fbx";
    auto rep_path = work_dir / L"report.json";

    auto cleanup = [&]() {
        std::error_code cec;
        std::filesystem::remove_all(work_dir, cec);
    };

    write_canonical_payload(scene, opts, json_path, bin_path, err);
    if (!err.empty()) { cleanup(); return false; }

    std::wstring cmd = quote_arg(binfo.exe_path.wstring()) +
                       L" --background --factory-startup --python " + quote_arg(script.wstring()) +
                       L" -- --mode export" +
                       L" --input " + quote_arg(json_path.wstring()) +
                       L" --bin " + quote_arg(bin_path.wstring()) +
                       L" --fbx " + quote_arg(tmp_fbx.wstring()) +
                       L" --report " + quote_arg(rep_path.wstring());
    if (opts.validate_output) cmd += L" --validate-after";
    if (!opts.render_preview_dir.empty()) cmd += L" --render-dir " + quote_arg(opts.render_preview_dir.wstring());

    std::string proc_out;
    DWORD exit_code = 1;
    if (!run_process_capture(cmd, proc_out, exit_code, cancel_flag, 240000)) {
        cleanup();
        err = "FBX export cancelled by user";
        return false;
    }

    if (!std::filesystem::exists(rep_path, ec)) {
        cleanup();
        err = "Blender FBX backend exited (code " + std::to_string(exit_code) + ") without report. Output tail: " +
              (proc_out.size() > 600 ? proc_out.substr(proc_out.size() - 600) : proc_out);
        return false;
    }

    std::vector<uint8_t> rep_bytes;
    if (!read_file_bytes(rep_path, rep_bytes, err)) { cleanup(); return false; }
    std::string rep_str(rep_bytes.begin(), rep_bytes.end());
    Json rep;
    std::string jerr;
    if (!Json::parse(rep_str, rep, jerr)) {
        cleanup();
        err = "Failed to parse Blender FBX report JSON: " + jerr;
        return false;
    }

    if (!rep.get("ok") || !rep.get("ok")->as_bool()) {
        std::string berr = rep.get("error") ? rep.get("error")->as_str() : "Unknown Blender export failure";
        cleanup();
        err = "Blender FBX export failed: " + berr;
        return false;
    }

    // Populate numeric round-trip validation metrics
    auto st = scene.compute_stats();
    out_val.src_meshes = st.mesh_count;
    out_val.src_triangles = st.total_triangles;
    out_val.src_nodes = st.node_count;
    out_val.src_materials = st.material_count;
    out_val.src_bones = st.bone_count;
    out_val.src_anim_clips = st.animation_clip_count;

    double src_motion = 0.0;
    for (const auto& clip : scene.animations) {
        for (const auto& tr : clip.tracks) {
            for (size_t i = 1; i < tr.translations.size(); ++i) {
                const auto& a = tr.translations[i - 1].value;
                const auto& b = tr.translations[i].value;
                src_motion = std::max(src_motion, double((a - b).length()));
            }
            for (size_t i = 1; i < tr.rotations.size(); ++i) {
                const auto& a = tr.rotations[i - 1].value;
                const auto& b = tr.rotations[i].value;
                float dot = std::fabs(a.w*b.w + a.x*b.x + a.y*b.y + a.z*b.z);
                src_motion = std::max(src_motion, 2.0 * std::acos(std::min(1.0, std::max(-1.0, double(dot)))));
            }
            for (size_t i = 1; i < tr.scales.size(); ++i) {
                const auto& a = tr.scales[i - 1].value;
                const auto& b = tr.scales[i].value;
                src_motion = std::max(src_motion, double((a - b).length()));
            }
        }
    }

    // Canonical hierarchy signature for strict parent/child comparison.
    std::vector<std::string> src_sig;
    for (const auto& n : scene.nodes) {
        if (n.is_hardpoint) continue; // hardpoint attachment links excluded by documented policy
        std::string parent = "-";
        if (n.parent_index >= 0 && n.parent_index < static_cast<int>(scene.nodes.size())) {
            const Node& pn = scene.nodes[n.parent_index];
            // FBX normalization: bone-parented mesh nodes collapse to the
            // armature root. Represent both sides with the same explicit marker.
            bool parent_is_bone = false;
            if (n.mesh_index >= 0 || n.is_hardpoint) {
                int cur = n.parent_index;
                int hops = 0;
                while (cur >= 0 && cur < static_cast<int>(scene.nodes.size()) && hops++ < 64) {
                    if (scene.nodes[cur].is_bone || scene.nodes[cur].mesh_index >= 0) {
                        if (scene.nodes[cur].is_bone || (cur != n.parent_index && scene.nodes[cur].mesh_index >= 0)) {
                            parent_is_bone = true;
                            break;
                        }
                    }
                    cur = scene.nodes[cur].parent_index;
                }
            }
            if (parent_is_bone) src_sig.push_back(n.name + "<-" + pn.name);
            parent = parent_is_bone ? "ARMATURE" : pn.name;
        }
        src_sig.push_back(n.name + "<-" + parent);
    }
    // Collapse optional aliases: one signature per node, preferring the
    // explicit ARMATURE normalization when both forms were generated.
    std::sort(src_sig.begin(), src_sig.end());
    src_sig.erase(std::unique(src_sig.begin(), src_sig.end()), src_sig.end());
    for (size_t i = 0; i < src_sig.size();) {
        std::string name = src_sig[i].substr(0, src_sig[i].find('<'));
        bool armour = src_sig[i].size() >= 9 && src_sig[i].compare(src_sig[i].size()-9,9,"<-ARMATURE") == 0;
        bool keep = true;
        if (!armour) {
            for (const auto& other : src_sig)
                if (other != src_sig[i] && other.substr(0, other.find('<')) == name &&
                    other.size() >= 9 && other.compare(other.size()-9,9,"<-ARMATURE") == 0) { keep = false; break; }
        }
        if (keep) ++i; else src_sig.erase(src_sig.begin() + i);
    }

    if (const Json* rm = rep.get("reimportMetrics")) {
        out_val.ran = true;
        out_val.dst_meshes = static_cast<size_t>(rm->get("meshCount")->as_int());
        out_val.dst_triangles = static_cast<size_t>(rm->get("triangleCount")->as_int());
        out_val.dst_nodes = static_cast<size_t>(rm->get("objectCount")->as_int());
        out_val.dst_materials = static_cast<size_t>(rm->get("materialCount")->as_int());
        out_val.dst_bones = static_cast<size_t>(rm->get("boneCount")->as_int());
        out_val.dst_anim_clips = static_cast<size_t>(rm->get("actionCount")->as_int());
        out_val.normal_agreement = rm->get("normalAgreement") ? rm->get("normalAgreement")->as_num(1.0) : 1.0;
        out_val.uv_present = rm->get("uvPresent") && rm->get("uvPresent")->as_bool();
        out_val.skin_weights_present = rm->get("skinWeightsPresent") && rm->get("skinWeightsPresent")->as_bool();
        out_val.anim_sampled = rm->get("animSampled") && rm->get("animSampled")->as_bool();
        out_val.anim_motion = rm->get("animMotion") ? rm->get("animMotion")->as_num(0.0) : 0.0;

        // Strict hierarchy/parent comparison. Blender may add one armature wrapper;
        // that wrapper is allowed, but no original node may lose its parent link.
        std::map<std::string, std::set<std::string>> src_parents, dst_parents;
        auto add_sig = [](std::map<std::string, std::set<std::string>>& m, const std::string& sig) {
            size_t sep = sig.find("<-");
            if (sep != std::string::npos) m[sig.substr(0, sep)].insert(sig.substr(sep + 2));
        };
        for (const auto& sig : src_sig) add_sig(src_parents, sig);
        if (const Json* hs = rm->get("hierarchySignature"))
            for (const auto& e : hs->as_arr()) add_sig(dst_parents, e.as_str());
        size_t hm = 0;
        for (const auto& [node, parents] : src_parents) {
            auto it = dst_parents.find(node);
            if (it == dst_parents.end()) continue;
            for (const auto& p : parents) {
                if (it->second.count(p)) { ++hm; break; }
            }
        }
        out_val.hierarchy_agreement = src_parents.empty() ? 1.0 : double(hm) / double(src_parents.size());
        out_val.hierarchy_ok = out_val.hierarchy_agreement >= 0.95;

        if (const Json* wf = rep.get("weightFidelity")) {
            out_val.weight_agreement = wf->get("agreement") ? wf->get("agreement")->as_num(1.0) : 1.0;
            out_val.weight_sum_error = wf->get("weightSumError") ? wf->get("weightSumError")->as_num(0.0) : 0.0;
            out_val.src_max_influences = static_cast<size_t>(wf->get("srcMaxInfluences") ? wf->get("srcMaxInfluences")->as_int() : 0);
            out_val.dst_max_influences = static_cast<size_t>(wf->get("dstMaxInfluences") ? wf->get("dstMaxInfluences")->as_int() : 0);
            out_val.src_weighted_vertices = static_cast<size_t>(wf->get("srcWeightedVertices") ? wf->get("srcWeightedVertices")->as_int() : 0);
            out_val.dst_weighted_vertices = static_cast<size_t>(wf->get("dstWeightedVertices") ? wf->get("dstWeightedVertices")->as_int() : 0);
        }

        if (!out_val.hierarchy_ok) {
            out_val.issues.push_back("Hierarchy parent links changed: agreement " +
                std::to_string(out_val.hierarchy_agreement));
        }
        if (st.material_count > 0 && out_val.dst_materials == 0 && opts.include_materials) {
            out_val.materials_ok = false;
            out_val.issues.push_back("Source had materials, re-imported FBX has none");
        }
        if (st.skinned_mesh_count > 0) {
            if (!out_val.skin_weights_present) {
                out_val.skeleton_ok = false;
                out_val.issues.push_back("Source was skinned, but re-imported FBX has no vertex weights");
            }
            if (out_val.dst_bones < st.bone_count) {
                out_val.skeleton_ok = false;
                out_val.issues.push_back("Bone count dropped: src=" + std::to_string(st.bone_count) + " dst=" + std::to_string(out_val.dst_bones));
            }
            if (out_val.src_weighted_vertices > 0 && (out_val.weight_agreement < 0.95 ||
                out_val.dst_max_influences < out_val.src_max_influences ||
                out_val.weight_sum_error > 0.02)) {
                out_val.skeleton_ok = false;
                std::string sample;
                if (const Json* us = rep.get("weightFidelity")->get("unmatchedSamples")) {
                    for (size_t i = 0; i < std::min<size_t>(3, us->as_arr().size()); ++i) {
                        if (i) sample += ", ";
                        sample += us->as_arr()[i].as_str();
                    }
                }
                out_val.issues.push_back("Skin weight fidelity failed: agreement=" + std::to_string(out_val.weight_agreement) +
                    " srcMax=" + std::to_string(out_val.src_max_influences) + " dstMax=" + std::to_string(out_val.dst_max_influences) +
                    " sumErr=" + std::to_string(out_val.weight_sum_error) +
                    (sample.empty() ? "" : " unmatched e.g. " + sample));
            }
        }
        if (st.animation_clip_count > 0 && opts.include_animations) {
            if (out_val.dst_anim_clips == 0 || !out_val.anim_sampled) {
                out_val.animations_ok = false;
                out_val.issues.push_back("Source had animations, re-imported FBX has no sampled actions");
            } else if (src_motion > 1e-5 && out_val.anim_motion < 1e-5) {
                double src_bl_motion = rep.get("sourceMetrics") && rep.get("sourceMetrics")->get("animMotion")
                    ? rep.get("sourceMetrics")->get("animMotion")->as_num(0.0) : 0.0;
                if (src_bl_motion > 1e-5) {
                    out_val.animations_ok = false;
                    out_val.issues.push_back("Animation actions present but no measurable pose change between sampled frames");
                } else {
                    out_val.issues.push_back("Animation tracks exist but contain no transform deltas representable in FBX; classification kept PARTIAL");
                    out_val.animations_ok = false;
                }
            }
        }

        adjudicate_fbx_validation(out_val);
        out_val.summary = out_val.passed
            ? ("FBX verified via Blender re-import: tris=" + std::to_string(out_val.dst_triangles) +
               " meshes=" + std::to_string(out_val.dst_meshes) +
               " bones=" + std::to_string(out_val.dst_bones) +
               " actions=" + std::to_string(out_val.dst_anim_clips) +
               " bboxErr=" + std::to_string(out_val.bbox_max_error))
            : ("FBX re-import validation flagged " + std::to_string(out_val.issues.size()) + " issue(s)");
    }

    // Crash-safe atomic move of validated FBX to final destination
    std::vector<uint8_t> fbx_bytes;
    if (!read_file_bytes(tmp_fbx, fbx_bytes, err)) { cleanup(); return false; }
    bool wrote = write_file_atomic(out_fbx_path, fbx_bytes.data(), fbx_bytes.size(), err);
    cleanup();
    return wrote;
}

bool validate_fbx_file(const std::filesystem::path& fbx_path, const std::filesystem::path& blender_override,
                       const std::filesystem::path& bridge_override, ValidationMetrics& out_val,
                       std::string& err, const std::filesystem::path& render_dir) {
    std::error_code ec;
    if (!std::filesystem::exists(fbx_path, ec)) { err = "FBX file does not exist: " + utf8_from_path(fbx_path); return false; }
    auto binfo = detect_blender(blender_override);
    if (!binfo.found) { err = binfo.diagnostic; return false; }
    auto script = locate_bridge_script(bridge_override);
    if (script.empty()) { err = "bz_fbx_bridge.py not found"; return false; }

    auto uid = std::chrono::steady_clock::now().time_since_epoch().count();
    auto rep_path = std::filesystem::temp_directory_path(ec) / (L"bzconv_val_" + std::to_wstring(uid) + L".json");

    std::wstring cmd = quote_arg(binfo.exe_path.wstring()) +
                       L" --background --factory-startup --python " + quote_arg(script.wstring()) +
                       L" -- --mode validate --fbx " + quote_arg(fbx_path.wstring()) +
                       L" --report " + quote_arg(rep_path.wstring());
    if (!render_dir.empty()) cmd += L" --render-dir " + quote_arg(render_dir.wstring());

    std::string proc_out;
    DWORD rc = 1;
    run_process_capture(cmd, proc_out, rc, nullptr, 120000);
    if (!std::filesystem::exists(rep_path, ec)) {
        err = "Blender validation failed (code " + std::to_string(rc) + ")";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(rep_path, bytes, err)) { std::filesystem::remove(rep_path, ec); return false; }
    std::filesystem::remove(rep_path, ec);

    Json rep;
    std::string jerr;
    if (!Json::parse(std::string(bytes.begin(), bytes.end()), rep, jerr) || !rep.get("ok") || !rep.get("ok")->as_bool()) {
        err = rep.get("error") ? rep.get("error")->as_str() : ("FBX re-import failed: " + jerr);
        return false;
    }
    if (const Json* rm = rep.get("reimportMetrics")) {
        out_val.ran = true;
        out_val.dst_meshes = static_cast<size_t>(rm->get("meshCount")->as_int());
        out_val.dst_triangles = static_cast<size_t>(rm->get("triangleCount")->as_int());
        out_val.dst_nodes = static_cast<size_t>(rm->get("objectCount")->as_int());
        out_val.dst_materials = static_cast<size_t>(rm->get("materialCount")->as_int());
        out_val.dst_bones = static_cast<size_t>(rm->get("boneCount")->as_int());
        out_val.dst_anim_clips = static_cast<size_t>(rm->get("actionCount")->as_int());
        out_val.normal_agreement = rm->get("normalAgreement") ? rm->get("normalAgreement")->as_num(1.0) : 1.0;
        out_val.uv_present = rm->get("uvPresent") && rm->get("uvPresent")->as_bool();
        out_val.skin_weights_present = rm->get("skinWeightsPresent") && rm->get("skinWeightsPresent")->as_bool();
        out_val.anim_sampled = rm->get("animSampled") && rm->get("animSampled")->as_bool();
        out_val.passed = (out_val.dst_meshes > 0 && out_val.dst_triangles > 0);
        out_val.summary = "Re-imported FBX: meshes=" + std::to_string(out_val.dst_meshes) +
                          " tris=" + std::to_string(out_val.dst_triangles) +
                          " mats=" + std::to_string(out_val.dst_materials) +
                          " bones=" + std::to_string(out_val.dst_bones) +
                          " actions=" + std::to_string(out_val.dst_anim_clips);
    }
    return true;
}

void adjudicate_fbx_validation(ValidationMetrics& out_val) {
    // A single failed feature or any recorded issue fails verification (no false green).
    out_val.passed = out_val.geometry_ok && out_val.normals_ok && out_val.uvs_ok &&
                     out_val.hierarchy_ok && out_val.materials_ok && out_val.skeleton_ok &&
                     out_val.animations_ok && out_val.issues.empty();
}

} // namespace bz
