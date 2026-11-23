#pragma once
#include "bzconverter/scene/scene.hpp"
#include <atomic>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace bz {

struct ResolverConfig {
    std::vector<std::filesystem::path> user_search_roots;
    std::vector<std::filesystem::path> game_asset_roots;
    std::filesystem::path workshop_root;
    std::filesystem::path cache_dir;
    bool search_workshop = false;
    bool decode_to_png = true;
    // Optional explicit user overrides: lowercase texture basename or ref -> full file path
    std::map<std::string, std::string> texture_overrides;
};

struct MaterialResolutionSummary {
    size_t materials_total = 0;
    size_t material_files_loaded = 0;
    size_t diffuse_requested = 0;
    size_t diffuse_resolved = 0;
    size_t diffuse_missing = 0;
    size_t emissive_resolved = 0;
    size_t normal_resolved = 0;
    size_t specular_resolved = 0;
    size_t team_color_resolved = 0;
    size_t ambiguous_matches = 0;
    size_t decode_failures = 0;
};

class MaterialResolver {
public:
    explicit MaterialResolver(ResolverConfig cfg = {});

    void set_config(const ResolverConfig& cfg);
    const ResolverConfig& config() const { return cfg_; }

    // Build/refresh bounded index of search roots (with optional cancellation)
    void ensure_index(std::atomic<bool>* cancel_flag = nullptr);
    void clear_index();
    size_t indexed_file_count() const { return indexed_count_; }

    // Parse a BZCC .material INI file into a Material struct
    static bool parse_material_sidecar(const std::filesystem::path& mat_path, Material& mat, std::string& err);

    // Resolve all materials and textures on a Scene in-place
    MaterialResolutionSummary resolve_scene(Scene& scene, std::atomic<bool>* cancel_flag = nullptr);

    // Default known Battlezone directories detected on this machine
    static std::vector<std::filesystem::path> detect_default_game_roots();
    static std::filesystem::path detect_default_workshop_root();

private:
    ResolverConfig cfg_;
    bool index_built_ = false;
    size_t indexed_count_ = 0;

    // Indexed by lowercase stem -> list of (priority_tier, full_path)
    // Priority tiers: lower = higher priority
    struct IndexedEntry {
        int tier = 10;
        std::string ext_lower;
        std::filesystem::path path;
    };
    std::unordered_map<std::string, std::vector<IndexedEntry>> tex_index_;
    std::unordered_map<std::string, std::vector<IndexedEntry>> mat_index_;

    void index_directory(const std::filesystem::path& root, int tier, int max_depth, std::atomic<bool>* cancel_flag);
    std::filesystem::path find_material_file(const std::string& mat_ref, const std::filesystem::path& model_dir);
    bool resolve_texture_channel(TextureChannel& ch, const std::filesystem::path& model_dir,
                                 const std::string& stem_hint, Scene& scene, bool is_team_color);
    std::vector<IndexedEntry> gather_local_candidates(const std::filesystem::path& model_dir,
                                                      const std::string& stem_lower);
    static std::string extract_basename_stem(const std::string& raw_ref);
    static int ext_priority(const std::string& ext_lower);
};

} // namespace bz
