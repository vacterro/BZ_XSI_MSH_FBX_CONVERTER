#pragma once
#include "bzconverter/exporters/exporter.hpp"

namespace bz {

bool export_scene_obj(const Scene& scene, const std::filesystem::path& out_obj_path,
                      const ExportOptions& opts, std::vector<Diagnostic>& diags, std::string& err,
                      std::atomic<bool>* cancel_flag = nullptr);

} // namespace bz
