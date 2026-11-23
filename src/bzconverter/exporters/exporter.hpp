#pragma once
#include "bzconverter/materials/material_resolver.hpp"
#include "bzconverter/scene/scene.hpp"
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace bz {

enum class ExportFormat {
    FBX,
    GLB,
    GLTF,
    OBJ
};

inline const char* export_format_to_string(ExportFormat f) {
    switch (f) {
        case ExportFormat::FBX: return "fbx";
        case ExportFormat::GLB: return "glb";
        case ExportFormat::GLTF: return "gltf";
        case ExportFormat::OBJ: return "obj";
    }
    return "fbx";
}

inline ExportFormat export_format_from_string(const std::string& s) {
    std::string lo = to_lower(trim(s));
    if (lo == "glb") return ExportFormat::GLB;
    if (lo == "gltf") return ExportFormat::GLTF;
    if (lo == "obj") return ExportFormat::OBJ;
    return ExportFormat::FBX;
}

enum class ConversionProfile {
    StandardFBX,
    GeometryOnly,
    GeometryMaterials,
    FullScene,
    BlenderCompat,
    GLBPortable,
    Custom
};

inline const char* profile_to_string(ConversionProfile p) {
    switch (p) {
        case ConversionProfile::StandardFBX: return "Standard FBX";
        case ConversionProfile::GeometryOnly: return "Geometry Only";
        case ConversionProfile::GeometryMaterials: return "Geometry + Materials";
        case ConversionProfile::FullScene: return "Full Available Scene";
        case ConversionProfile::BlenderCompat: return "Blender / BZCC Compat";
        case ConversionProfile::GLBPortable: return "GLB Portable";
        case ConversionProfile::Custom: return "Custom";
    }
    return "Standard FBX";
}

struct ExportOptions {
    ExportFormat format = ExportFormat::FBX;
    ConversionProfile profile = ConversionProfile::FullScene;
    bool include_textures = true;
    bool include_materials = true;
    bool include_animations = true;
    bool preserve_hierarchy = true;
    bool embed_textures = true;
    bool validate_output = true;
    bool overwrite = false; // Never replace an existing output unless explicitly allowed
    size_t glb_max_influences = 0; // 0 = strict fidelity (no truncation); >0 = explicit compatibility limit
    bool bzcc_axis_mode = false; // If true, exports FBX with BZCC Forward=-Y Up=Z, Apply Scaling=All
    std::filesystem::path companion_model; // Base model for animation-only XSI assets
    std::filesystem::path blender_exe; // Optional explicit override
    std::filesystem::path bridge_script; // Path to bz_fbx_bridge.py
    std::filesystem::path render_preview_dir; // Optional visual regression render output
};

struct FeatureCoverage {
    bool geometry = true;
    bool normals = true;
    bool uvs = true;
    bool vertex_colors = true;
    bool hierarchy = true;
    bool materials = true;
    bool textures = true;
    bool skeleton = true;
    bool animations = true;
    std::string notes;
};

inline FeatureCoverage get_feature_coverage(ExportFormat fmt) {
    switch (fmt) {
        case ExportFormat::FBX:
            return {true, true, true, true, true, true, true, true, true,
                    "Full scene via Blender FBX backend (meshes, custom normals, UVs, vertex colors, hierarchy, materials/textures, armature skinning, animations)"};
        case ExportFormat::GLB:
        case ExportFormat::GLTF:
            return {true, true, true, true, true, true, true, true, true,
                    "Native C++ glTF 2.0 / GLB (meshes, normals, UVs, vertex colors, hierarchy, PBR materials + embedded PNG textures, skins, animations; zero external dependencies)"};
        case ExportFormat::OBJ:
            return {true, true, true, false, false, true, true, false, false,
                    "Native C++ Wavefront OBJ + MTL (geometry, normals, UVs, materials/textures only; hierarchy flattened, skeleton & animations NOT supported by OBJ)"};
    }
    return {};
}

struct ValidationMetrics {
    bool ran = false;
    bool passed = false;
    size_t src_meshes = 0, dst_meshes = 0;
    size_t src_triangles = 0, dst_triangles = 0;
    size_t src_nodes = 0, dst_nodes = 0;
    size_t src_materials = 0, dst_materials = 0;
    size_t src_bones = 0, dst_bones = 0;
    size_t src_anim_clips = 0, dst_anim_clips = 0;
    double bbox_max_error = 0.0;
    double normal_agreement = 1.0;
    bool uv_present = false;
    bool skin_weights_present = false;
    bool anim_sampled = false;

    // Per-feature fidelity. A false value on any requested feature fails verification.
    bool geometry_ok = true;
    bool normals_ok = true;
    bool uvs_ok = true;
    bool hierarchy_ok = true;
    bool materials_ok = true;
    bool skeleton_ok = true;
    bool animations_ok = true;
    double weight_agreement = 1.0;
    double weight_sum_error = 0.0;
    double hierarchy_agreement = 1.0;
    double anim_motion = 0.0;
    double bind_matrix_error = 0.0;
    size_t src_max_influences = 0, dst_max_influences = 0;
    size_t src_weighted_vertices = 0, dst_weighted_vertices = 0;

    std::string summary;
    std::vector<std::string> issues;
};

struct ExportResult {
    JobStatus status = JobStatus::Failed;
    ResultClass result_class = ResultClass::Failed;
    std::string output_path;
    std::string summary;
    MaterialResolutionSummary material_summary;
    ValidationMetrics validation;
    std::vector<Diagnostic> diagnostics;
    double elapsed_ms = 0.0;
};

} // namespace bz
