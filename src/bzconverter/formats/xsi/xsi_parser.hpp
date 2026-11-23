#pragma once
#include "bzconverter/scene/scene.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace bz {

struct XsiParseOptions {
    bool include_animations = true;
    bool include_envelopes = true;
    bool include_vertex_colors = true;
};

bool parse_xsi_bytes(const std::vector<uint8_t>& bytes, const std::filesystem::path& source_path,
                     Scene& out_scene, std::string& out_error, const XsiParseOptions& opts = {});

bool parse_xsi_file(const std::filesystem::path& path, Scene& out_scene, std::string& out_error,
                    const XsiParseOptions& opts = {});

} // namespace bz
