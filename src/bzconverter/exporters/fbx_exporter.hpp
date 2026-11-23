#pragma once
#include "bzconverter/exporters/exporter.hpp"
#include <atomic>
#include <filesystem>
#include <string>

namespace bz {

struct BlenderInfo {
    bool found = false;
    std::filesystem::path exe_path;
    std::string version_string;
    std::string diagnostic;
};

BlenderInfo detect_blender(const std::filesystem::path& explicit_override = {});
std::filesystem::path locate_bridge_script(const std::filesystem::path& explicit_override = {});

bool export_scene_fbx(const Scene& scene, const std::filesystem::path& out_fbx_path,
                      const ExportOptions& opts, ValidationMetrics& out_val,
                      std::vector<Diagnostic>& diags, std::string& err,
                      std::atomic<bool>* cancel_flag = nullptr);

// Single authoritative adjudication of per-feature fidelity. Any false feature
// or issue fails verification; kept as a function so regression tests can prove
// no issue can be reported with passed=true.
void adjudicate_fbx_validation(ValidationMetrics& out_val);

bool validate_fbx_file(const std::filesystem::path& fbx_path, const std::filesystem::path& blender_override,
                       const std::filesystem::path& bridge_override, ValidationMetrics& out_val,
                       std::string& err, const std::filesystem::path& render_dir = {});

} // namespace bz
