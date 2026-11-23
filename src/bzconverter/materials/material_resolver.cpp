#include "bzconverter/materials/material_resolver.hpp"
#include "bzconverter/materials/texture_codec.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>

namespace bz {

namespace {

bool is_texture_ext(const std::string& ext) {
    return ext == ".dds" || ext == ".png" || ext == ".tga" || ext == ".pic" ||
           ext == ".dxtbz2" || ext == ".bmp" || ext == ".jpg" || ext == ".jpeg";
}

void parse_floats_from_str(const std::string& s, float* out, int max_n) {
    std::istringstream is(s);
    for (int i = 0; i < max_n && (is >> out[i]); ++i) {}
}

} // namespace

MaterialResolver::MaterialResolver(ResolverConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.cache_dir.empty()) {
        std::error_code ec;
        cfg_.cache_dir = std::filesystem::temp_directory_path(ec) / "bzconverter_tex_cache";
    }
}

void MaterialResolver::set_config(const ResolverConfig& cfg) {
    cfg_ = cfg;
    clear_index();
}

void MaterialResolver::clear_index() {
    index_built_ = false;
    indexed_count_ = 0;
    tex_index_.clear();
    mat_index_.clear();
}

int MaterialResolver::ext_priority(const std::string& ext_lower) {
    // Prefer modern high-res lossless/BZCC assets first, then classic BZ2
    if (ext_lower == ".png") return 0;
    if (ext_lower == ".dds") return 1;
    if (ext_lower == ".tga") return 2;
    if (ext_lower == ".dxtbz2") return 3;
    if (ext_lower == ".pic") return 4;
    if (ext_lower == ".bmp") return 5;
    if (ext_lower == ".jpg" || ext_lower == ".jpeg") return 6;
    return 99;
}

std::string MaterialResolver::extract_basename_stem(const std::string& raw_ref) {
    std::string s = trim(raw_ref);
    // Strip quotes
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'')) s = s.substr(1, s.size() - 2);
    // Normalize UNC / forward / back slashes (e.g., //SERVER/Battlezone/.../fvtank00.pic or Z:/.../foo.pic)
    std::replace(s.begin(), s.end(), '\\', '/');
    auto slash = s.rfind('/');
    if (slash != std::string::npos) s = s.substr(slash + 1);
    auto dot = s.rfind('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    return to_lower(s);
}

std::vector<std::filesystem::path> MaterialResolver::detect_default_game_roots() {
    std::vector<std::filesystem::path> out;
    const char* candidates[] = {
        "V:\\__STORE_V\\_SOFT\\Steam\\steamapps\\common\\BZ2R\\bz2r_res",
        "P:\\__SAVE_P\\___VAC\\_BATTLEZONE\\_BZCC\\_RAW\\bz2r_res",
        "V:\\___VAC\\_BATTLEZONE\\_BZ2",
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\BZ2R\\bz2r_res",
        "C:\\Program Files\\Steam\\steamapps\\common\\BZ2R\\bz2r_res"
    };
    std::error_code ec;
    for (const char* c : candidates) {
        std::filesystem::path p(c);
        if (std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec)) {
            out.push_back(p);
        }
    }
    return out;
}

std::filesystem::path MaterialResolver::detect_default_workshop_root() {
    const char* candidates[] = {
        "V:\\__STORE_V\\_SOFT\\Steam\\steamapps\\workshop\\content\\624970",
        "C:\\Program Files (x86)\\Steam\\steamapps\\workshop\\content\\624970"
    };
    std::error_code ec;
    for (const char* c : candidates) {
        std::filesystem::path p(c);
        if (std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec)) return p;
    }
    return {};
}

void MaterialResolver::index_directory(const std::filesystem::path& root, int tier, int max_depth,
                                       std::atomic<bool>* cancel_flag) {
    std::error_code ec;
    if (root.empty() || !std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) return;
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        if (cancel_flag && cancel_flag->load()) return;
        if (it.depth() > max_depth) { it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(ec)) continue;
        auto p = it->path();
        std::string ext = to_lower(utf8_from_path(p.extension()));
        if (is_texture_ext(ext)) {
            std::string stem = to_lower(utf8_from_path(p.stem()));
            tex_index_[stem].push_back({tier, ext, p});
            ++indexed_count_;
        } else if (ext == ".material") {
            std::string fname = to_lower(utf8_from_path(p.filename()));
            mat_index_[fname].push_back({tier, ext, p});
            ++indexed_count_;
        }
    }
}

void MaterialResolver::ensure_index(std::atomic<bool>* cancel_flag) {
    if (index_built_) return;
    // Tier 3: User search roots
    for (const auto& r : cfg_.user_search_roots) index_directory(r, 3, 6, cancel_flag);
    // Tier 5: Known game asset roots
    for (const auto& r : cfg_.game_asset_roots) index_directory(r, 5, 7, cancel_flag);
    // Tier 6: Steam Workshop (only when enabled, bounded depth)
    if (cfg_.search_workshop && !cfg_.workshop_root.empty()) {
        index_directory(cfg_.workshop_root, 6, 5, cancel_flag);
    }
    if (!cancel_flag || !cancel_flag->load()) index_built_ = true;
}

bool MaterialResolver::parse_material_sidecar(const std::filesystem::path& mat_path, Material& mat, std::string& err) {
    std::ifstream in(mat_path);
    if (!in) { err = "Failed to open .material: " + utf8_from_path(mat_path); return false; }
    std::string line, section;
    while (std::getline(in, line)) {
        auto comment = line.find("//");
        if (comment != std::string::npos) line.resize(comment);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = to_lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = to_lower(trim(line.substr(0, eq)));
        std::string val = trim(line.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);

        if (section == "solid") {
            if (key == "diffuse") { float v[4]={mat.diffuse.x,mat.diffuse.y,mat.diffuse.z,mat.diffuse.w}; parse_floats_from_str(val, v, 4); mat.diffuse={v[0],v[1],v[2],v[3]}; if (v[3] < 0.999f) mat.has_alpha = true; }
            else if (key == "specular") { float v[4]={mat.specular.x,mat.specular.y,mat.specular.z,mat.specular.w}; parse_floats_from_str(val, v, 4); mat.specular={v[0],v[1],v[2],v[3]}; }
            else if (key == "specularpower") { float v[1]={mat.specular_power}; parse_floats_from_str(val, v, 1); mat.specular_power = v[0]; }
            else if (key == "emissive") { float v[4]={mat.emissive.x,mat.emissive.y,mat.emissive.z,mat.emissive.w}; parse_floats_from_str(val, v, 4); mat.emissive={v[0],v[1],v[2],v[3]}; }
            else if (key == "ambient") { float v[4]={mat.ambient.x,mat.ambient.y,mat.ambient.z,mat.ambient.w}; parse_floats_from_str(val, v, 4); mat.ambient={v[0],v[1],v[2],v[3]}; }
        } else if (section == "texture") {
            if (key == "diffuse") { mat.diffuse_tex.original_ref = val; mat.diffuse_tex.is_srgb = true; }
            else if (key == "emissive") { mat.emissive_tex.original_ref = val; mat.emissive_tex.is_srgb = true; }
            else if (key == "normal") { mat.normal_tex.original_ref = val; mat.normal_tex.is_srgb = false; }
            else if (key == "specular") { mat.specular_tex.original_ref = val; mat.specular_tex.is_srgb = false; }
            else if (key == "teamcolor") { mat.team_color_tex.original_ref = val; mat.team_color_tex.is_srgb = false; }
        }
    }
    mat.material_file_path = utf8_from_path(mat_path);
    return true;
}

std::filesystem::path MaterialResolver::find_material_file(const std::string& mat_ref, const std::filesystem::path& model_dir) {
    if (mat_ref.empty()) return {};
    std::string fname = to_lower(mat_ref);
    std::error_code ec;
    // Priority 1: model directory (case-insensitive scan of immediate folder)
    if (!model_dir.empty() && std::filesystem::exists(model_dir, ec)) {
        auto direct = model_dir / path_from_utf8(mat_ref);
        if (std::filesystem::exists(direct, ec) && std::filesystem::is_regular_file(direct, ec)) return direct;
        for (const auto& e : std::filesystem::directory_iterator(model_dir, ec)) {
            if (e.is_regular_file(ec) && to_lower(utf8_from_path(e.path().filename())) == fname) return e.path();
        }
    }
    // Indexed search
    auto it = mat_index_.find(fname);
    if (it != mat_index_.end() && !it->second.empty()) {
        auto best = std::min_element(it->second.begin(), it->second.end(),
            [](const IndexedEntry& a, const IndexedEntry& b) { return a.tier < b.tier; });
        return best->path;
    }
    return {};
}

std::vector<MaterialResolver::IndexedEntry> MaterialResolver::gather_local_candidates(
    const std::filesystem::path& model_dir, const std::string& stem_lower) {
    std::vector<IndexedEntry> out;
    if (model_dir.empty() || stem_lower.empty()) return out;
    std::error_code ec;

    auto scan_dir = [&](const std::filesystem::path& dir, int tier) {
        if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) return;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec)) continue;
            std::string ext = to_lower(utf8_from_path(e.path().extension()));
            if (!is_texture_ext(ext)) continue;
            if (to_lower(utf8_from_path(e.path().stem())) == stem_lower) {
                out.push_back({tier, ext, e.path()});
            }
        }
    };

    // Tier 1: Source model directory
    scan_dir(model_dir, 1);
    // Tier 2: Nearest textures/texture/images/pictures subdirectories (same level & parent)
    const char* subnames[] = {"textures", "texture", "images", "pictures"};
    for (const char* sn : subnames) {
        scan_dir(model_dir / sn, 2);
        if (model_dir.has_parent_path()) scan_dir(model_dir.parent_path() / sn, 2);
    }
    return out;
}

bool MaterialResolver::resolve_texture_channel(TextureChannel& ch, const std::filesystem::path& model_dir,
                                               const std::string& stem_hint, Scene& scene, bool is_team_color) {
    std::string stem = !ch.original_ref.empty() ? extract_basename_stem(ch.original_ref) : to_lower(stem_hint);
    if (stem.empty()) return false;

    // Never accidentally resolve a _c team-color texture as a diffuse map!
    if (!is_team_color && stem.size() > 2 && stem.rfind("_c") == stem.size() - 2 && ch.original_ref.empty()) {
        return false;
    }

    // Check explicit user overrides first
    for (const auto& key : {to_lower(ch.original_ref), stem}) {
        auto oit = cfg_.texture_overrides.find(key);
        if (oit != cfg_.texture_overrides.end()) {
            std::error_code ec;
            auto p = path_from_utf8(oit->second);
            if (std::filesystem::exists(p, ec)) {
                ch.resolved_path = utf8_from_path(p);
                return true;
            }
        }
    }

    // Direct absolute/relative path check (only if not legacy //SERVER or Z:/)
    if (!ch.original_ref.empty() && ch.original_ref.rfind("//", 0) != 0 && ch.original_ref.rfind("\\\\", 0) != 0) {
        std::error_code ec;
        auto direct = path_from_utf8(ch.original_ref);
        if (direct.is_absolute() && std::filesystem::exists(direct, ec) && std::filesystem::is_regular_file(direct, ec)) {
            ch.resolved_path = utf8_from_path(direct);
            return true;
        }
    }

    std::vector<IndexedEntry> cands = gather_local_candidates(model_dir, stem);
    // Also check BZCC diffuse naming convention: fvconst00.pic -> fvconst00_d.dds
    if (cands.empty() && !is_team_color && stem.find('_') == std::string::npos) {
        auto alt = gather_local_candidates(model_dir, stem + "_d");
        cands.insert(cands.end(), alt.begin(), alt.end());
    }
    // Check global indexed roots
    auto it = tex_index_.find(stem);
    if (it != tex_index_.end()) cands.insert(cands.end(), it->second.begin(), it->second.end());
    if (!is_team_color && stem.find('_') == std::string::npos) {
        auto it_d = tex_index_.find(stem + "_d");
        if (it_d != tex_index_.end()) cands.insert(cands.end(), it_d->second.begin(), it_d->second.end());
    }

    if (cands.empty()) return false;

    // Deduplicate paths (case-insensitive)
    std::sort(cands.begin(), cands.end(), [](const IndexedEntry& a, const IndexedEntry& b) {
        if (a.tier != b.tier) return a.tier < b.tier;
        int pa = ext_priority(a.ext_lower), pb = ext_priority(b.ext_lower);
        if (pa != pb) return pa < pb;
        return to_lower(utf8_from_path(a.path)) < to_lower(utf8_from_path(b.path));
    });

    std::vector<IndexedEntry> unique;
    std::set<std::string> seen;
    for (const auto& e : cands) {
        std::string k = to_lower(utf8_from_path(e.path));
        if (seen.insert(k).second) unique.push_back(e);
    }

    // Ambiguity detection: multiple distinct candidates in the SAME winning priority tier and different folders/mods
    int best_tier = unique[0].tier;
    int best_ext_p = ext_priority(unique[0].ext_lower);
    std::set<std::string> same_tier_dirs;
    for (const auto& u : unique) {
        if (u.tier == best_tier && ext_priority(u.ext_lower) == best_ext_p) {
            same_tier_dirs.insert(to_lower(utf8_from_path(u.path.parent_path())));
        }
        if (ch.candidates.size() < 8) ch.candidates.push_back(utf8_from_path(u.path));
    }
    if (same_tier_dirs.size() > 1 && best_tier >= 5) {
        ch.ambiguous = true;
        scene.add_diag(DiagSeverity::Warning, "TEXTURE_AMBIGUOUS",
            "Ambiguous matches (" + std::to_string(same_tier_dirs.size()) + " folders at tier " +
            std::to_string(best_tier) + ") for texture '" + stem + "'; picked '" +
            utf8_from_path(unique[0].path) + "' (override supported)");
    }

    ch.resolved_path = utf8_from_path(unique[0].path);
    return true;
}

MaterialResolutionSummary MaterialResolver::resolve_scene(Scene& scene, std::atomic<bool>* cancel_flag) {
    ensure_index(cancel_flag);
    MaterialResolutionSummary sum;
    sum.materials_total = scene.materials.size();

    std::filesystem::path model_dir = path_from_utf8(scene.source_path).parent_path();

    for (auto& mat : scene.materials) {
        if (cancel_flag && cancel_flag->load()) break;

        // 1. Load BZCC .material sidecar if referenced
        if (!mat.material_file_ref.empty()) {
            auto mp = find_material_file(mat.material_file_ref, model_dir);
            if (!mp.empty()) {
                std::string merr;
                if (parse_material_sidecar(mp, mat, merr)) {
                    ++sum.material_files_loaded;
                } else {
                    scene.add_diag(DiagSeverity::Warning, "MATERIAL_SIDECAR_ERR", merr);
                }
            } else {
                scene.add_diag(DiagSeverity::Warning, "MATERIAL_SIDECAR_MISSING",
                    "Referenced .material sidecar not found: " + mat.material_file_ref);
            }
        }

        // 2. Resolve diffuse texture
        if (!mat.diffuse_tex.original_ref.empty()) {
            ++sum.diffuse_requested;
            if (resolve_texture_channel(mat.diffuse_tex, model_dir, "", scene, false)) {
                ++sum.diffuse_resolved;
                if (mat.diffuse_tex.ambiguous) ++sum.ambiguous_matches;
            } else {
                ++sum.diffuse_missing;
                scene.add_diag(DiagSeverity::Warning, "TEXTURE_MISSING",
                    "Missing diffuse texture '" + mat.diffuse_tex.original_ref + "' for material '" + mat.name + "'");
            }
        }

        // 3. Discover BZCC companion PBR/channel suffixes (_e, _n, _s, _c) if diffuse was found
        std::string base_stem;
        if (!mat.diffuse_tex.resolved_path.empty()) {
            base_stem = to_lower(utf8_from_path(path_from_utf8(mat.diffuse_tex.resolved_path).stem()));
            if (base_stem.size() > 2 && base_stem.rfind("_d") == base_stem.size() - 2)
                base_stem = base_stem.substr(0, base_stem.size() - 2);
        }

        if (!mat.emissive_tex.original_ref.empty() || !base_stem.empty()) {
            mat.emissive_tex.is_srgb = true;
            if (resolve_texture_channel(mat.emissive_tex, model_dir, base_stem.empty() ? "" : base_stem + "_e", scene, false))
                ++sum.emissive_resolved;
        }
        if (!mat.normal_tex.original_ref.empty() || !base_stem.empty()) {
            mat.normal_tex.is_srgb = false;
            if (resolve_texture_channel(mat.normal_tex, model_dir, base_stem.empty() ? "" : base_stem + "_n", scene, false))
                ++sum.normal_resolved;
        }
        if (!mat.specular_tex.original_ref.empty() || !base_stem.empty()) {
            mat.specular_tex.is_srgb = false;
            if (resolve_texture_channel(mat.specular_tex, model_dir, base_stem.empty() ? "" : base_stem + "_s", scene, false))
                ++sum.specular_resolved;
        }
        if (!mat.team_color_tex.original_ref.empty() || !base_stem.empty()) {
            mat.team_color_tex.is_srgb = false;
            if (resolve_texture_channel(mat.team_color_tex, model_dir, base_stem.empty() ? "" : base_stem + "_c", scene, true))
                ++sum.team_color_resolved;
        }

        // 4. Stage/decode resolved textures to standard PNG if enabled
        if (cfg_.decode_to_png) {
            auto stage_ch = [&](TextureChannel& ch, bool check_alpha) {
                if (ch.resolved_path.empty()) return;
                std::filesystem::path png_out;
                bool alpha = false;
                std::string derr;
                if (stage_texture_as_png(path_from_utf8(ch.resolved_path), cfg_.cache_dir, png_out, alpha, derr)) {
                    ch.converted_png_path = utf8_from_path(png_out);
                    if (check_alpha && alpha) mat.has_alpha = true;
                } else {
                    ++sum.decode_failures;
                    scene.add_diag(DiagSeverity::Warning, "TEXTURE_DECODE_FAILED",
                        "Could not decode texture '" + ch.resolved_path + "': " + derr);
                }
            };
            stage_ch(mat.diffuse_tex, true);
            stage_ch(mat.emissive_tex, false);
            stage_ch(mat.normal_tex, false);
            stage_ch(mat.specular_tex, false);
            stage_ch(mat.team_color_tex, false);
        }
    }
    return sum;
}

} // namespace bz
