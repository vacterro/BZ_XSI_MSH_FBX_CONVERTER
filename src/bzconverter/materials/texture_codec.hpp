#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bz {

struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // width * height * 4, top-to-bottom RGBA8
    bool has_non_opaque_alpha = false;
    std::string source_format;
};

// Decode supported Battlezone / standard image formats (.pic, .dxtbz2, .dds, .tga, .bmp)
bool decode_image_file(const std::filesystem::path& path, DecodedImage& out, std::string& err);
bool decode_image_bytes(const std::vector<uint8_t>& bytes, const std::string& ext_hint,
                        DecodedImage& out, std::string& err);

// Write RGBA8 image as valid PNG (via zlib) using atomic write
bool write_png_file(const std::filesystem::path& path, int width, int height,
                    const uint8_t* rgba, std::string& err);
bool encode_png_bytes(int width, int height, const uint8_t* rgba,
                      std::vector<uint8_t>& out_png, std::string& err);

// Convert/stage texture as PNG (or copy if already .png/.jpg) into out_dir.
// Never renames unknown binaries to PNG; strictly validates headers.
bool stage_texture_as_png(const std::filesystem::path& src_path,
                          const std::filesystem::path& cache_dir,
                          std::filesystem::path& out_png_path,
                          bool& out_has_alpha,
                          std::string& err);

} // namespace bz
