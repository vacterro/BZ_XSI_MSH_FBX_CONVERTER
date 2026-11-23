#pragma once
#include "bzconverter/core/types.hpp"
#include "bzconverter/formats/probe.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bz {

struct TextureChannel {
    std::string original_ref;        // Raw reference from XSI / MSH / .material
    std::string resolved_path;       // Absolute path on disk if found
    std::string converted_png_path;  // Standard PNG path when decoded from .pic/.dxtbz2/.dds
    bool is_srgb = true;
    bool ambiguous = false;
    std::vector<std::string> candidates;
};

struct Material {
    std::string name;
    std::string material_file_ref;   // e.g. ivtank00_1.material (BZCC)
    std::string material_file_path;  // Resolved .material path if found

    Vec4 diffuse = {0.7f, 0.7f, 0.7f, 1.0f};
    Vec4 specular = {0.5f, 0.5f, 0.5f, 1.0f};
    float specular_power = 32.0f;
    Vec4 emissive = {0.0f, 0.0f, 0.0f, 1.0f};
    Vec4 ambient = {0.5f, 0.5f, 0.5f, 1.0f};
    int shading_type = 2;

    bool double_sided = false;
    bool has_alpha = false;
    bool emissive_flag = false;
    float emissive_strength = 1.0f;
    bool no_texture_flag = false;

    TextureChannel diffuse_tex;
    TextureChannel emissive_tex;
    TextureChannel normal_tex;
    TextureChannel specular_tex;
    TextureChannel team_color_tex;   // Preserved separately, NEVER forced into diffuse!
};

struct BoneWeight {
    int bone_node_index = -1;
    float weight = 0.0f; // Normalized [0, 1]
};

struct MeshVertex {
    Vec3 pos;
    Vec3 normal = {0, 1, 0};
    Vec2 uv = {0, 0};
    Vec4 color = {1, 1, 1, 1};
    // Optional link back to source control-point vertex index (for skin weights)
    int source_vertex_index = -1;
};

struct Mesh {
    std::string name;
    // Triangle-ready corner vertices (deduplicated per (pos, normal, uv, color, source_v))
    std::vector<MeshVertex> vertices;
    // Triangle indices (3 per triangle)
    std::vector<uint32_t> indices;
    // Material index per triangle (into Scene::materials), -1 if unassigned
    std::vector<int> triangle_materials;
    // Source polygon count before triangulation (for diagnostics)
    size_t source_polygon_count = 0;
    size_t max_polygon_arity = 3;
    bool has_normals = false;
    bool has_uvs = false;
    bool has_vertex_colors = false;
    // Skinning: weights per MeshVertex (aligned with vertices.size() if skinned)
    bool is_skinned = false;
    std::vector<std::vector<BoneWeight>> skin_weights;
    // Whether vertex coordinates are already in model/world space (e.g. skinned MSH combined geometry)
    bool vertices_in_model_space = false;
};

struct Node {
    std::string name;
    std::string raw_name;
    int parent_index = -1;
    std::vector<int> children;

    // Local transform (row-vector convention: v_parent = v_local * local_matrix)
    Mat4 local_matrix = Mat4::identity();
    // Base pose local transform (from SI_FrameBasePoseMatrix or MSH pose)
    bool has_base_pose = false;
    Mat4 base_pose_matrix = Mat4::identity();
    // Inverse bind matrix (row-vector convention: v_bone = v_model * inverse_bind_matrix)
    bool has_inverse_bind = false;
    Mat4 inverse_bind_matrix = Mat4::identity();

    // Computed cached world transform (row-vector)
    Mat4 world_matrix = Mat4::identity();

    // Mesh index in Scene::meshes, or -1 for empty/bone/hardpoint
    int mesh_index = -1;
    bool is_bone = false;
    bool is_hardpoint = false;
    bool is_collision = false;
    bool is_hidden = false;
    std::string bz_flags;
};

struct KeyframeVec3 {
    float frame = 0.0f;
    Vec3 value;
};

struct KeyframeQuat {
    float frame = 0.0f;
    // Stored as standard right-handed column-vector quaternion (w, x, y, z)!
    Quat value;
};

struct AnimTrack {
    int node_index = -1;
    std::string node_name;
    std::vector<KeyframeVec3> translations;
    std::vector<KeyframeQuat> rotations;
    std::vector<KeyframeVec3> scales;
};

struct AnimationClip {
    std::string name;
    float fps = 20.0f; // Battlezone 2/BZCC standard 20 FPS
    float start_frame = 0.0f;
    float end_frame = 0.0f;
    std::vector<AnimTrack> tracks;
};

struct SceneStats {
    size_t node_count = 0;
    size_t mesh_count = 0;
    size_t bone_count = 0;
    size_t hardpoint_count = 0;
    size_t total_vertices = 0;
    size_t total_triangles = 0;
    size_t source_polygons = 0;
    size_t material_count = 0;
    size_t animation_clip_count = 0;
    size_t animation_track_count = 0;
    size_t skinned_mesh_count = 0;
    bool has_uvs = false;
    bool has_normals = false;
    bool has_vertex_colors = false;
    BBox world_bbox;
    double normal_face_agreement = 1.0; // Share of triangles whose normals match face winding
    size_t degenerate_triangles = 0;
    size_t negative_det_nodes = 0;
};

struct Scene {
    std::string source_path;
    std::string source_name;
    SourceFormat source_format = SourceFormat::Unknown;
    std::string source_sha256;
    float source_scale = 1.0f;
    // UV origin note: true if UV.y=0 is bottom (OpenGL/Softimage/Blender), false if UV.y=0 is top (Direct3D)
    bool uv_origin_bottom_left = true;
    bool is_animation_only = false;

    std::vector<Node> nodes;
    std::vector<int> root_nodes;
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<AnimationClip> animations;
    std::vector<Diagnostic> diagnostics;

    void add_diag(DiagSeverity sev, std::string code, std::string msg) {
        diagnostics.push_back({sev, std::move(code), std::move(msg)});
    }

    int find_node(const std::string& name) const {
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].name == name) return static_cast<int>(i);
        return -1;
    }

    int find_node_ci(const std::string& name) const {
        std::string lo = to_lower(name);
        for (size_t i = 0; i < nodes.size(); ++i)
            if (to_lower(nodes[i].name) == lo) return static_cast<int>(i);
        return -1;
    }

    void update_world_matrices();
    SceneStats compute_stats() const;
    void normalize_skin_weights();
    void ensure_inverse_bind_matrices();
};

} // namespace bz
