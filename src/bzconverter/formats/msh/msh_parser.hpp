#pragma once
#include "bzconverter/scene/scene.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace bz {

struct MshParseOptions {
    bool include_animations = true;
    bool flip_uv_v = true; // DOCB bakes Direct3D UV (v=0 top); flip to canonical bottom-left v=1-v
};

bool parse_msh_bytes(const std::vector<uint8_t>& bytes, const std::filesystem::path& source_path,
                     Scene& out_scene, std::string& out_error, const MshParseOptions& opts = {});

bool parse_msh_file(const std::filesystem::path& path, Scene& out_scene, std::string& out_error,
                    const MshParseOptions& opts = {});

} // namespace bz
