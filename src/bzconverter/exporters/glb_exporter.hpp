#pragma once
#include "bzconverter/exporters/exporter.hpp"
#include <unordered_map>

namespace bz {

struct SkinInfluence {
    uint16_t joint = 0;
    float weight = 0.0f;
};

// Valid (mapped, positive, finite, deduplicated) influences for one vertex,
// sorted by descending weight with joint index as tie-break. When limit > 0
// the list is capped to exactly that many influences before normalization.
std::vector<SkinInfluence> select_skin_influences(
    const std::vector<BoneWeight>& weights,
    const std::unordered_map<int, uint16_t>& node_to_joint,
    size_t limit);

// Native C++ glTF 2.0 / GLB exporter (zero external dependencies).
// Preserves hierarchy, meshes, normals, UVs, vertex colors, PBR materials + embedded PNG textures,
// skeletons/skins (JOINTS_0, WEIGHTS_0, inverseBindMatrices), and animations!
bool export_scene_gltf(const Scene& scene, const std::filesystem::path& out_path,
                       bool binary_glb, const ExportOptions& opts,
                       std::vector<Diagnostic>& diags, std::string& err,
                       std::atomic<bool>* cancel_flag = nullptr);

// Packs one vertex's influences into glTF JOINTS_0/WEIGHTS_0 (4 slots): keeps the
// 4 strongest weights whose bone maps to a joint, renormalized to sum 1.
// No usable influence -> joint 0 with weight 1.
void pack_skin_influences(const std::vector<BoneWeight>& weights,
                          const std::unordered_map<int, uint16_t>& node_to_joint,
                          uint16_t joints_out[4], float weights_out[4]);

// Extended packing: all mapped positive influences up to `slots`, normalized to
// sum 1 across the whole list. Unweighted vertices keep all weights at zero.
void pack_skin_influences_ext(const std::vector<BoneWeight>& weights,
                             const std::unordered_map<int, uint16_t>& node_to_joint,
                             uint16_t* joints_out, float* weights_out, size_t slots);

} // namespace bz
