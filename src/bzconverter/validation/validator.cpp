#include "bzconverter/validation/validator.hpp"
#include "bzconverter/core/json.hpp"
#include "bzconverter/exporters/fbx_exporter.hpp"
#include "bzconverter/exporters/glb_exporter.hpp"
#include "bzconverter/exporters/obj_exporter.hpp"
#include "bzconverter/formats/msh/msh_parser.hpp"
#include "bzconverter/formats/xsi/xsi_parser.hpp"
#include <chrono>
#include <algorithm>
#include <unordered_map>

namespace bz {

std::string node_key(const std::string& name);
std::string node_key(const std::string& name) {
    std::string k = to_lower(trim(name));
    while (!k.empty() && (k.front() == '*' || k.front() == '_' || k.front() == '.')) k.erase(k.begin());
    std::replace(k.begin(), k.end(), ':', '_');
    return k;
}


bool build_companion_node_map(const Scene& anim_scene, const Scene& base_scene,
                              std::unordered_map<int, int>& mapping, std::string& reason) {
    std::unordered_map<std::string, std::vector<int>> by_name;
    for (size_t i = 0; i < base_scene.nodes.size(); ++i)
        by_name[node_key(base_scene.nodes[i].name)].push_back(static_cast<int>(i));

    std::vector<const AnimTrack*> tracks;
    for (const auto& clip : anim_scene.animations)
        for (const auto& tr : clip.tracks) tracks.push_back(&tr);
    float diag = std::max(1.0f, base_scene.compute_stats().world_bbox.diagonal());
    for (const auto* tr : tracks) {
        if (tr->node_index < 0 || tr->node_index >= static_cast<int>(anim_scene.nodes.size())) {
            reason = "animation track has invalid source node index " + std::to_string(tr->node_index);
            return false;
        }
        const Node& src = anim_scene.nodes[tr->node_index];
        auto it = by_name.find(node_key(src.name));
        if (it == by_name.end() || it->second.empty()) {
            reason = "no companion node named '" + src.name + "' for animated node";
            return false;
        }
        int target = -1;
        if (it->second.size() == 1) {
            target = it->second.front();
        } else {
            for (int candidate : it->second) {
                const Node& b = base_scene.nodes[candidate];
                bool parent_ok = (src.parent_index < 0 && b.parent_index < 0) ||
                    (src.parent_index >= 0 && b.parent_index >= 0 &&
                     src.parent_index < static_cast<int>(anim_scene.nodes.size()) &&
                     node_key(anim_scene.nodes[src.parent_index].name) ==
                     node_key(base_scene.nodes[b.parent_index].name));
                bool bind_ok = !src.has_inverse_bind || !b.has_inverse_bind ||
                    (src.inverse_bind_matrix.transform_point({0,0,0}) -
                     b.inverse_bind_matrix.transform_point({0,0,0})).length() <= diag * 0.02f;
                if (parent_ok && bind_ok) { target = candidate; break; }
            }
            if (target < 0) {
                reason = "ambiguous companion nodes named '" + src.name + "' with no matching hierarchy/bind pose";
                return false;
            }
            int second = -1;
            for (int candidate : it->second) {
                if (candidate == target) continue;
                const Node& b = base_scene.nodes[candidate];
                bool parent_ok = (src.parent_index < 0 && b.parent_index < 0) ||
                    (src.parent_index >= 0 && b.parent_index >= 0 &&
                     src.parent_index < static_cast<int>(anim_scene.nodes.size()) &&
                     node_key(anim_scene.nodes[src.parent_index].name) == node_key(base_scene.nodes[b.parent_index].name));
                bool bind_ok = !src.has_inverse_bind || !b.has_inverse_bind ||
                    (src.inverse_bind_matrix.transform_point({0,0,0}) - b.inverse_bind_matrix.transform_point({0,0,0})).length() <= diag * 0.02f;
                if (parent_ok && bind_ok) { second = candidate; break; }
            }
            if (second >= 0) {
                reason = "ambiguous companion nodes named '" + src.name + "' - multiple matches with equal hierarchy/bind evidence";
                return false;
            }
        }
        const Node& b = base_scene.nodes[target];
        if (src.has_inverse_bind && b.has_inverse_bind &&
            (src.inverse_bind_matrix.transform_point({0,0,0}) - b.inverse_bind_matrix.transform_point({0,0,0})).length() > diag * 0.05f) {
            reason = "bind pose mismatch for animated node '" + src.name + "'";
            return false;
        }
        mapping[tr->node_index] = target;
    }
    // Coverage is over unique source node identities: several clips may
    // animate the same bone. Every track still went through matching above.
    std::vector<int> unique_sources;
    for (const auto* tr : tracks) {
        if (std::find(unique_sources.begin(), unique_sources.end(), tr->node_index) == unique_sources.end()) {
            unique_sources.push_back(tr->node_index);
        }
    }
    if (mapping.size() < unique_sources.size()) { reason = "incomplete animation-to-companion node coverage"; return false; }
    return true;
}

namespace {

bool skeleton_compatible(const Scene& anim_scene, const Scene& base_scene, std::string& reason) {
    if (base_scene.meshes.empty()) { reason = "companion has no meshes"; return false; }
    if (base_scene.nodes.empty()) { reason = "companion has no nodes"; return false; }
    std::vector<const AnimTrack*> tracks;
    for (const auto& clip : anim_scene.animations)
        for (const auto& tr : clip.tracks) tracks.push_back(&tr);
    if (tracks.empty()) { reason = "animation-only source has no tracks"; return false; }

    std::vector<int> unique_sources;
    for (const auto* tr : tracks) {
        if (tr->node_index < 0 || tr->node_index >= static_cast<int>(anim_scene.nodes.size())) continue;
        if (std::find(unique_sources.begin(), unique_sources.end(), tr->node_index) == unique_sources.end()) {
            unique_sources.push_back(tr->node_index);
        }
    }
    size_t matched = 0;
    for (int source_idx : unique_sources) {
        if (source_idx < 0 || source_idx >= static_cast<int>(anim_scene.nodes.size())) continue;
        const std::string& name = anim_scene.nodes[source_idx].name;
        int bi = base_scene.find_node_ci(name);
        if (bi < 0) continue;
        const Node& a = anim_scene.nodes[source_idx];
        const Node& b = base_scene.nodes[bi];
        if (!a.has_inverse_bind || !b.has_inverse_bind) { ++matched; continue; }
        Vec3 pa = a.inverse_bind_matrix.transform_point({0, 0, 0});
        Vec3 pb = b.inverse_bind_matrix.transform_point({0, 0, 0});
        float dist = (pa - pb).length();
        float diag = std::max(1.0f, base_scene.compute_stats().world_bbox.diagonal());
        if (dist > diag * 0.02f) continue;
        ++matched;
    }
    double agreement = unique_sources.empty() ? 0.0 : double(matched) / double(unique_sources.size());
    if (agreement < 0.95) {
        reason = "only " + std::to_string(int(agreement * 100)) + "% of animation tracks match companion bones/bind poses";
        return false;
    }
    return true;
}

std::vector<std::filesystem::path> companion_candidates(const std::filesystem::path& input_path,
                                                        const ExportOptions& opts) {
    std::vector<std::filesystem::path> out;
    if (!opts.companion_model.empty()) { out.push_back(opts.companion_model); return out; }
    std::error_code ec;
    std::filesystem::path dir = input_path.parent_path();
    if (dir.empty()) dir = std::filesystem::current_path();
    std::string stem = bz::to_lower(input_path.stem().string());
    const char* suffixes[] = {"_walk", "_morph", "_idle", "_run", "_death01", "_death02", "_death03",
                              "_turn", "_attack", "_reload", "_anim", "_a"};
    std::vector<std::string> bases;
    for (const char* s : suffixes) {
        auto pos = stem.rfind(s);
        if (pos != std::string::npos && pos > 0) bases.push_back(stem.substr(0, pos));
    }
    for (const auto& base : bases) {
        for (const char* ext : {".xsi", ".msh"}) {
            std::filesystem::path p = dir / (base + ext);
            if (std::filesystem::exists(p, ec)) out.push_back(p);
            std::filesystem::path sk = dir / (base + "_skel" + ext);
            if (std::filesystem::exists(sk, ec)) out.push_back(sk);
        }
    }
    // Explicit skeleton convention only, never fuzzy name similarity.
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) {
            std::string n = bz::to_lower(entry.path().stem().string());
            if (n.find("_skel") != std::string::npos) out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void classify_result(ExportResult& res, const ExportOptions& opts, bool verification_ran) {
    if (res.status == JobStatus::Cancelled) { res.result_class = ResultClass::Cancelled; return; }
    if (res.status == JobStatus::Failed) { res.result_class = ResultClass::Failed; return; }
    if (res.status == JobStatus::Partial) { res.result_class = ResultClass::Partial; return; }
    if (verification_ran && res.validation.passed) { res.result_class = ResultClass::VerifiedSuccess; return; }
    if (!verification_ran && opts.validate_output && opts.format == ExportFormat::FBX) {
        res.result_class = ResultClass::SuccessUnverified;
        return;
    }
    res.result_class = ResultClass::SuccessUnverified;
}

} // namespace

bool load_model_file(const std::filesystem::path& input_path, Scene& out_scene, ProbeResult& out_probe,
                     std::string& out_error, bool include_animations) {
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(input_path, bytes, out_error)) {
        out_probe.format = SourceFormat::Malformed;
        out_probe.detail = out_error;
        return false;
    }
    out_probe = probe_bytes(bytes, input_path);
    if (!out_probe.supported) {
        out_error = out_probe.detail;
        return false;
    }
    if (out_probe.format == SourceFormat::XSI_Text_11 || out_probe.format == SourceFormat::XSI_Text_13) {
        XsiParseOptions xopts;
        xopts.include_animations = include_animations;
        return parse_xsi_bytes(bytes, input_path, out_scene, out_error, xopts);
    }
    if (out_probe.format == SourceFormat::MSH_DOCB_V1_BZCC || out_probe.format == SourceFormat::MSH_DOCB_V1_BZ2) {
        MshParseOptions mopts;
        mopts.include_animations = include_animations;
        return parse_msh_bytes(bytes, input_path, out_scene, out_error, mopts);
    }
    out_error = "Unsupported format: " + std::string(format_to_string(out_probe.format));
    return false;
}

void validate_canonical_scene(Scene& scene) {
    auto st = scene.compute_stats();
    if (st.mesh_count == 0 || st.total_triangles == 0) {
        scene.add_diag(DiagSeverity::Warning, "SCENE_EMPTY_GEOMETRY", "Scene contains 0 triangles");
    }
    if (st.degenerate_triangles > 0) {
        scene.add_diag(DiagSeverity::Info, "DEGENERATE_TRIANGLES",
            "Scene contains " + std::to_string(st.degenerate_triangles) + " zero-area degenerate triangles");
    }
    if (st.negative_det_nodes > 0) {
        scene.add_diag(DiagSeverity::Warning, "NEGATIVE_SCALE_NODE",
            std::to_string(st.negative_det_nodes) + " node(s) have negative scale determinant; winding compensated on export");
    }
    if (st.has_normals && st.total_triangles > 0 && st.normal_face_agreement < 0.70) {
        scene.add_diag(DiagSeverity::Warning, "LOW_NORMAL_AGREEMENT",
            "Only " + std::to_string(int(st.normal_face_agreement * 100)) + "% of triangles agree with vertex normal orientation");
    }
}

bool paths_identical(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    bool a_exists = std::filesystem::exists(a, ec);
    bool b_exists = std::filesystem::exists(b, ec);
    if (a_exists && b_exists) {
        bool equiv = std::filesystem::equivalent(a, b, ec);
        if (!ec && equiv) return true;
    }
    auto ca = std::filesystem::weakly_canonical(a, ec);
    if (ec) ca = a;
    auto cb = std::filesystem::weakly_canonical(b, ec);
    if (ec) cb = b;
    std::string sa = to_lower(utf8_from_path(ca));
    std::string sb = to_lower(utf8_from_path(cb));
    std::replace(sa.begin(), sa.end(), '/', '\\');
    std::replace(sb.begin(), sb.end(), '/', '\\');
    return sa == sb;
}

ExportResult convert_single_file(const std::filesystem::path& input_path,
                                 const std::filesystem::path& output_path,
                                 const ExportOptions& opts,
                                 MaterialResolver& resolver,
                                 std::atomic<bool>* cancel_flag) {
    ExportResult res;
    res.output_path = utf8_from_path(output_path);
    auto t0 = std::chrono::steady_clock::now();

    auto finish_time = [&]() {
        res.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };

    if (cancel_flag && cancel_flag->load()) {
        res.status = JobStatus::Cancelled;
        res.result_class = ResultClass::Cancelled;
        res.summary = "Cancelled before start";
        finish_time();
        return res;
    }

    // Source data has absolute priority: never let an export replace its own
    // input, regardless of overwrite flag or path spelling/case/symlink alias.
    if (paths_identical(input_path, output_path)) {
        res.status = JobStatus::Failed;
        res.result_class = ResultClass::Failed;
        res.summary = "Input and output refer to the same file: " + utf8_from_path(input_path);
        res.diagnostics.push_back({DiagSeverity::Error, "INPUT_OUTPUT_IDENTITY", res.summary});
        finish_time();
        return res;
    }

    std::error_code out_ec;
    if (!opts.overwrite && std::filesystem::exists(output_path, out_ec)) {
        res.status = JobStatus::Failed;
        res.result_class = ResultClass::Failed;
        res.summary = "Output already exists; overwrite disabled: " + utf8_from_path(output_path);
        res.diagnostics.push_back({DiagSeverity::Error, "OUTPUT_EXISTS", res.summary});
        finish_time();
        return res;
    }

    Scene scene;
    ProbeResult probe;
    std::string err;
    if (!load_model_file(input_path, scene, probe, err, opts.include_animations)) {
        res.status = JobStatus::Failed;
        res.result_class = ResultClass::Failed;
        res.summary = err;
        res.diagnostics.push_back({DiagSeverity::Error, "LOAD_FAILED", err});
        finish_time();
        return res;
    }

    validate_canonical_scene(scene);

    // Animation-only XSI sources carry no visual mesh. Merge with a verified
    // compatible companion model (skeleton names + bind poses) or refuse with
    // NEEDS_COMPANION_ASSET. Never merge on name similarity alone.
    if (scene.is_animation_only) {
        std::vector<AnimationClip> anims = std::move(scene.animations);
        std::string merge_reason;
        bool merged = false;
        for (const auto& cand : companion_candidates(input_path, opts)) {
            Scene base;
            ProbeResult bp;
            std::string berr, cand_reason;
            if (!load_model_file(cand, base, bp, berr, false)) continue;
            if (base.is_animation_only) continue;
            Scene anim_view = scene;
            anim_view.animations = anims;
            if (!skeleton_compatible(anim_view, base, cand_reason)) { merge_reason = cand_reason; continue; }
            std::unordered_map<int, int> node_map;
            if (!build_companion_node_map(anim_view, base, node_map, cand_reason)) { merge_reason = cand_reason; continue; }
            std::vector<AnimationClip> remapped = anims;
            bool map_ok = true;
            for (auto& clip : remapped) {
                for (auto& tr : clip.tracks) {
                    auto it = node_map.find(tr.node_index);
                    if (it == node_map.end()) {
                        cand_reason = "no companion mapping for animation track node " + std::to_string(tr.node_index);
                        map_ok = false;
                        break;
                    }
                    tr.node_index = it->second;
                    tr.node_name = base.nodes[it->second].name;
                }
                if (!map_ok) break;
            }
            if (!map_ok) { merge_reason = cand_reason; continue; }
            Scene merged_scene = std::move(base);
            merged_scene.animations = std::move(remapped);
            merged_scene.source_name = scene.source_name;
            merged_scene.source_format = scene.source_format;
            merged_scene.source_path = scene.source_path;
            merged_scene.source_sha256 = scene.source_sha256;
            merged_scene.is_animation_only = false;
            merged_scene.diagnostics.push_back({DiagSeverity::Info, "COMPANION_MERGED",
                "Animation-only source merged with compatible companion: " + utf8_from_path(cand)});
            scene = std::move(merged_scene);
            merged = true;
            break;
        }
        if (!merged) {
            res.status = JobStatus::Failed;
            res.result_class = ResultClass::NeedsCompanionAsset;
            res.summary = "Animation-only asset needs a compatible base model (matched skeleton names and bind poses). " +
                          (merge_reason.empty() ? "No companion found." : merge_reason);
            res.diagnostics.push_back({DiagSeverity::Error, "NEEDS_COMPANION_ASSET", res.summary});
            finish_time();
            return res;
        }
    }

    if (cancel_flag && cancel_flag->load()) {
        res.status = JobStatus::Cancelled;
        res.result_class = ResultClass::Cancelled;
        res.summary = "Cancelled after parse";
        finish_time();
        return res;
    }

    if (opts.include_materials && opts.include_textures) {
        res.material_summary = resolver.resolve_scene(scene, cancel_flag);
    }

    if (cancel_flag && cancel_flag->load()) {
        res.status = JobStatus::Cancelled;
        res.result_class = ResultClass::Cancelled;
        res.summary = "Cancelled during material resolution";
        finish_time();
        return res;
    }

    bool exported = false;
    std::vector<Diagnostic> exp_diags;
    if (opts.format == ExportFormat::FBX) {
        exported = export_scene_fbx(scene, output_path, opts, res.validation, exp_diags, err, cancel_flag);
    } else if (opts.format == ExportFormat::GLB) {
        exported = export_scene_gltf(scene, output_path, true, opts, exp_diags, err, cancel_flag);
    } else if (opts.format == ExportFormat::GLTF) {
        exported = export_scene_gltf(scene, output_path, false, opts, exp_diags, err, cancel_flag);
    } else if (opts.format == ExportFormat::OBJ) {
        exported = export_scene_obj(scene, output_path, opts, exp_diags, err, cancel_flag);
    }

    res.diagnostics.insert(res.diagnostics.end(), scene.diagnostics.begin(), scene.diagnostics.end());
    res.diagnostics.insert(res.diagnostics.end(), exp_diags.begin(), exp_diags.end());
    for (const auto& issue : res.validation.issues) {
        res.diagnostics.push_back({DiagSeverity::Error, "FBX_ROUNDTRIP_FIDELITY", issue});
    }

    if (cancel_flag && cancel_flag->load()) {
        res.status = JobStatus::Cancelled;
        res.result_class = ResultClass::Cancelled;
        res.summary = "Cancelled during export";
        finish_time();
        return res;
    }

    if (!exported) {
        res.status = JobStatus::Failed;
        res.result_class = ResultClass::Failed;
        res.summary = err;
        res.diagnostics.push_back({DiagSeverity::Error, "EXPORT_FAILED", err});
        finish_time();
        return res;
    }

    adjudicate_result(res, opts, scene.compute_stats());
    finish_time();
    return res;
}

void adjudicate_result(ExportResult& res, const ExportOptions& opts, const SceneStats& st) {
    // Honest status adjudication: NEVER claim green SUCCESS when requested materials/textures are missing,
    // when OBJ dropped skeleton/animations, or when output validation flagged issues!
    bool partial = false;
    std::vector<std::string> partial_reasons;

    if (opts.include_materials && opts.include_textures && res.material_summary.diffuse_missing > 0) {
        partial = true;
        partial_reasons.push_back(std::to_string(res.material_summary.diffuse_missing) + " missing diffuse texture(s)");
    }
    if (opts.include_materials && opts.include_textures && res.material_summary.decode_failures > 0) {
        partial = true;
        partial_reasons.push_back(std::to_string(res.material_summary.decode_failures) + " texture decode failure(s)");
    }
    if (res.validation.ran && !res.validation.passed) {
        partial = true;
        partial_reasons.push_back("output validation flagged " + std::to_string(res.validation.issues.size()) + " issue(s)");
    }
    for (const auto& d : res.diagnostics) {
        if (diagnostic_forces_partial(d)) {
            partial = true;
            partial_reasons.push_back(d.code);
        }
    }

    if (partial) {
        res.status = JobStatus::Partial;
        res.result_class = ResultClass::Partial;
        std::string rstr;
        for (size_t i = 0; i < partial_reasons.size(); ++i) {
            if (i) rstr += "; ";
            rstr += partial_reasons[i];
        }
        res.summary = "PARTIAL (" + rstr + ") — tris=" + std::to_string(st.total_triangles) +
                      " nodes=" + std::to_string(st.node_count);
    } else {
        res.status = JobStatus::Success;
        classify_result(res, opts, res.validation.ran);
        res.summary = std::string(result_class_to_string(res.result_class)) + " — tris=" + std::to_string(st.total_triangles) +
                      " nodes=" + std::to_string(st.node_count) +
                      " mats=" + std::to_string(st.material_count) +
                      " anims=" + std::to_string(st.animation_clip_count);
    }
}

std::string build_inspect_json(const ProbeResult& probe, const Scene* scene, const std::string& parse_error) {
    Json root = Json::object();
    root["format"] = format_to_string(probe.format);
    root["supported"] = probe.supported;
    root["magic"] = probe.magic_summary;
    root["detail"] = probe.detail;
    root["fileSize"] = probe.file_size;
    root["sha256"] = probe.sha256;
    if (!parse_error.empty()) root["parseError"] = parse_error;

    if (scene) {
        auto st = scene->compute_stats();
        Json js = Json::object();
        js["nodes"] = st.node_count;
        js["meshes"] = st.mesh_count;
        js["bones"] = st.bone_count;
        js["hardpoints"] = st.hardpoint_count;
        js["vertices"] = st.total_vertices;
        js["triangles"] = st.total_triangles;
        js["sourcePolygons"] = st.source_polygons;
        js["materials"] = st.material_count;
        js["skinnedMeshes"] = st.skinned_mesh_count;
        js["animationClips"] = st.animation_clip_count;
        js["animationTracks"] = st.animation_track_count;
        js["hasUvs"] = st.has_uvs;
        js["hasNormals"] = st.has_normals;
        js["hasVertexColors"] = st.has_vertex_colors;
        js["normalFaceAgreement"] = st.normal_face_agreement;
        js["degenerateTriangles"] = st.degenerate_triangles;
        js["negativeDetNodes"] = st.negative_det_nodes;
        Json blo = Json::array(), bhi = Json::array();
        blo.push_back(st.world_bbox.lo.x); blo.push_back(st.world_bbox.lo.y); blo.push_back(st.world_bbox.lo.z);
        bhi.push_back(st.world_bbox.hi.x); bhi.push_back(st.world_bbox.hi.y); bhi.push_back(st.world_bbox.hi.z);
        js["bboxMin"] = std::move(blo);
        js["bboxMax"] = std::move(bhi);
        root["stats"] = std::move(js);

        Json jmats = Json::array();
        for (const auto& m : scene->materials) {
            Json jm = Json::object();
            jm["name"] = m.name;
            jm["materialFileRef"] = m.material_file_ref;
            jm["diffuseRef"] = m.diffuse_tex.original_ref;
            jm["diffuseResolved"] = m.diffuse_tex.resolved_path;
            jm["emissiveResolved"] = m.emissive_tex.resolved_path;
            jm["normalResolved"] = m.normal_tex.resolved_path;
            jm["specularResolved"] = m.specular_tex.resolved_path;
            jm["teamColorResolved"] = m.team_color_tex.resolved_path;
            jmats.push_back(std::move(jm));
        }
        root["materials"] = std::move(jmats);

        Json janims = Json::array();
        for (const auto& a : scene->animations) {
            Json ja = Json::object();
            ja["name"] = a.name;
            ja["fps"] = a.fps;
            ja["startFrame"] = a.start_frame;
            ja["endFrame"] = a.end_frame;
            ja["tracks"] = a.tracks.size();
            janims.push_back(std::move(ja));
        }
        root["animations"] = std::move(janims);

        Json jdiags = Json::array();
        for (const auto& d : scene->diagnostics) {
            Json jd = Json::object();
            jd["severity"] = (d.severity == DiagSeverity::Error) ? "ERROR" : (d.severity == DiagSeverity::Warning ? "WARN" : "INFO");
            jd["code"] = d.code;
            jd["message"] = d.message;
            jdiags.push_back(std::move(jd));
        }
        root["diagnostics"] = std::move(jdiags);
    }
    return root.dump(2);
}

} // namespace bz
