#include "bzconverter/exporters/obj_exporter.hpp"
#include "bzconverter/core/output_bundle.hpp"
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace bz {

namespace {

std::string sanitize_mtl_name(std::string s) {
    if (s.empty()) return "default_mat";
    for (char& c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '/' || c == '\\') c = '_';
    }
    return s;
}

// Stage a texture into the declared output bundle. Never writes to disk
// directly; readiness and overwrite policy are decided by the bundle.
std::string stage_sidecar_tex(const TextureChannel& ch, const std::filesystem::path& tex_out_dir,
                              OutputBundle& bundle, std::vector<Diagnostic>& diags) {
    std::string src = !ch.converted_png_path.empty() ? ch.converted_png_path : ch.resolved_path;
    if (src.empty()) return "";
    std::error_code ec;
    auto sp = path_from_utf8(src);
    if (!std::filesystem::exists(sp, ec)) return "";
    auto dst = tex_out_dir / sp.filename();
    std::vector<uint8_t> bytes;
    std::string err;
    if (!read_file_bytes(sp, bytes, err)) {
        diags.push_back({DiagSeverity::Warning, "OBJ_TEXTURE_STAGE_FAILED",
            "Cannot stage texture '" + src + "': " + err});
        return "";
    }
    bundle.add(dst, std::move(bytes), "OBJ texture");
    return utf8_from_path(tex_out_dir.filename() / sp.filename());
}

} // namespace

bool export_scene_obj(const Scene& scene, const std::filesystem::path& out_obj_path,
                      const ExportOptions& opts, std::vector<Diagnostic>& diags, std::string& err,
                      std::atomic<bool>* cancel_flag) {
    auto stats = scene.compute_stats();
    if (stats.bone_count > 0 || stats.skinned_mesh_count > 0 || stats.animation_clip_count > 0) {
        diags.push_back({DiagSeverity::Warning, "OBJ_FEATURE_LOSS",
            "OBJ format does not support skeletons, skin weights, or animations (dropped " +
            std::to_string(stats.bone_count) + " bones, " +
            std::to_string(stats.animation_clip_count) + " animation clips)"});
    }

    std::filesystem::path mtl_path = out_obj_path;
    mtl_path.replace_extension(L".mtl");
    std::filesystem::path tex_dir = out_obj_path.parent_path() / (out_obj_path.stem().wstring() + L"_textures");

    OutputBundle bundle;

    std::ostringstream obj, mtl;
    obj << "# Exported by BZ_XSI_MSH_FBX_CONVERTER\n";
    obj << "# Source: " << scene.source_name << " (" << format_to_string(scene.source_format) << ")\n";
    if (opts.include_materials && !scene.materials.empty()) {
        obj << "mtllib " << utf8_from_path(mtl_path.filename()) << "\n";
        mtl << "# Material library for " << scene.source_name << "\n";
        for (size_t i = 0; i < scene.materials.size(); ++i) {
            const auto& m = scene.materials[i];
            std::string mname = sanitize_mtl_name(m.name) + "_" + std::to_string(i);
            mtl << "\nnewmtl " << mname << "\n";
            mtl << "Ka " << m.ambient.x << " " << m.ambient.y << " " << m.ambient.z << "\n";
            mtl << "Kd " << m.diffuse.x << " " << m.diffuse.y << " " << m.diffuse.z << "\n";
            mtl << "Ks " << m.specular.x << " " << m.specular.y << " " << m.specular.z << "\n";
            mtl << "Ke " << m.emissive.x << " " << m.emissive.y << " " << m.emissive.z << "\n";
            mtl << "Ns " << std::clamp(m.specular_power, 1.0f, 1000.0f) << "\n";
            mtl << "d " << m.diffuse.w << "\n";
            mtl << "illum 2\n";
            if (opts.include_textures) {
                std::string kd = stage_sidecar_tex(m.diffuse_tex, tex_dir, bundle, diags);
                if (!kd.empty()) mtl << "map_Kd " << kd << "\n";
                std::string ke = stage_sidecar_tex(m.emissive_tex, tex_dir, bundle, diags);
                if (!ke.empty()) mtl << "map_Ke " << ke << "\n";
                std::string ks = stage_sidecar_tex(m.specular_tex, tex_dir, bundle, diags);
                if (!ks.empty()) mtl << "map_Ks " << ks << "\n";
                std::string bump = stage_sidecar_tex(m.normal_tex, tex_dir, bundle, diags);
                if (!bump.empty()) mtl << "map_Bump " << bump << "\n";
            }
        }
    }

    uint32_t v_offset = 1;
    char buf[128];
    for (const auto& node : scene.nodes) {
        if (node.mesh_index < 0 || node.mesh_index >= static_cast<int>(scene.meshes.size())) continue;
        const auto& mesh = scene.meshes[node.mesh_index];
        if (mesh.vertices.empty() || mesh.indices.empty()) continue;

        Mat4 wm = mesh.vertices_in_model_space ? Mat4::identity() : node.world_matrix;
        bool flip_winding = wm.det3x3() < 0.0f;
        obj << "\no " << sanitize_mtl_name(node.name) << "\n";

        for (const auto& v : mesh.vertices) {
            Vec3 wp = wm.transform_point(v.pos);
            std::snprintf(buf, sizeof(buf), "v %.6f %.6f %.6f\n", wp.x, wp.y, wp.z);
            obj << buf;
        }
        for (const auto& v : mesh.vertices) {
            std::snprintf(buf, sizeof(buf), "vt %.6f %.6f\n", v.uv.x, v.uv.y);
            obj << buf;
        }
        for (const auto& v : mesh.vertices) {
            Vec3 wn = wm.transform_vector(v.normal).normalized();
            std::snprintf(buf, sizeof(buf), "vn %.6f %.6f %.6f\n", wn.x, wn.y, wn.z);
            obj << buf;
        }

        int cur_mat = -2;
        size_t ntris = mesh.indices.size() / 3;
        for (size_t t = 0; t < ntris; ++t) {
            int mi = (t < mesh.triangle_materials.size()) ? mesh.triangle_materials[t] : -1;
            if (opts.include_materials && mi != cur_mat) {
                cur_mat = mi;
                if (mi >= 0 && mi < static_cast<int>(scene.materials.size())) {
                    obj << "usemtl " << sanitize_mtl_name(scene.materials[mi].name) << "_" << mi << "\n";
                }
            }
            uint32_t a = v_offset + mesh.indices[t * 3 + 0];
            uint32_t b = v_offset + mesh.indices[t * 3 + (flip_winding ? 2 : 1)];
            uint32_t c = v_offset + mesh.indices[t * 3 + (flip_winding ? 1 : 2)];
            std::snprintf(buf, sizeof(buf), "f %u/%u/%u %u/%u/%u %u/%u/%u\n", a, a, a, b, b, b, c, c, c);
            obj << buf;
        }
        v_offset += static_cast<uint32_t>(mesh.vertices.size());
    }

    if (opts.include_materials && !scene.materials.empty()) {
        std::string ms = mtl.str();
        bundle.add(mtl_path, std::vector<uint8_t>(ms.begin(), ms.end()), "OBJ material library");
    }
    std::string os = obj.str();
    bundle.add(out_obj_path, std::vector<uint8_t>(os.begin(), os.end()), "OBJ document");
    return bundle.commit(opts.overwrite, cancel_flag, err);
}

} // namespace bz
