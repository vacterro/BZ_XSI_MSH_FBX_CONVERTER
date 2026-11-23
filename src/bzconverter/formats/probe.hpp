#pragma once
#include "bzconverter/core/types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace bz {

enum class SourceFormat {
    Unknown,
    XSI_Text_11,
    XSI_Text_13,
    MSH_DOCB_V1_BZCC,
    MSH_DOCB_V1_BZ2,
    MSH_SWBF_MSH2_Unsupported,
    MSH_UnknownVariant_Unsupported,
    Malformed
};

inline const char* format_to_string(SourceFormat f) {
    switch (f) {
        case SourceFormat::XSI_Text_11: return "XSI_1.1_TEXT";
        case SourceFormat::XSI_Text_13: return "XSI_1.3_TEXT";
        case SourceFormat::MSH_DOCB_V1_BZCC: return "MSH_DOCB_V1_BZCC";
        case SourceFormat::MSH_DOCB_V1_BZ2: return "MSH_DOCB_V1_BZ2";
        case SourceFormat::MSH_SWBF_MSH2_Unsupported: return "MSH_SWBF_MSH2_UNSUPPORTED";
        case SourceFormat::MSH_UnknownVariant_Unsupported: return "MSH_UNKNOWN_VARIANT";
        case SourceFormat::Malformed: return "MALFORMED";
        default: return "UNKNOWN";
    }
}

struct ProbeResult {
    SourceFormat format = SourceFormat::Unknown;
    bool supported = false;
    std::string magic_summary;
    std::string detail;
    uint32_t major_version = 0;
    uint32_t minor_version = 0;
    size_t file_size = 0;
    std::string sha256;
};

ProbeResult probe_bytes(const std::vector<uint8_t>& bytes, const std::filesystem::path& path_hint = {});
ProbeResult probe_file(const std::filesystem::path& path);

} // namespace bz
