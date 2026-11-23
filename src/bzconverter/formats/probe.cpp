#include "bzconverter/formats/probe.hpp"
#include "bzconverter/core/sha256.hpp"
#include <cstdio>
#include <cstring>

namespace bz {

ProbeResult probe_bytes(const std::vector<uint8_t>& bytes, const std::filesystem::path& path_hint) {
    ProbeResult r;
    r.file_size = bytes.size();
    if (!bytes.empty()) r.sha256 = sha256_hex(bytes.data(), bytes.size());

    if (bytes.size() < 4) {
        r.format = SourceFormat::Malformed;
        r.detail = "File too small (< 4 bytes)";
        return r;
    }

    // 1. Check ASCII dotXSI header: "xsi 0101txt 0032" or "xsi 0103txt ..."
    size_t off = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) off = 3;
    while (off < bytes.size() && (bytes[off] == ' ' || bytes[off] == '\t' || bytes[off] == '\r' || bytes[off] == '\n')) ++off;
    if (off + 11 <= bytes.size() &&
        (bytes[off] == 'x' || bytes[off] == 'X') &&
        (bytes[off+1] == 's' || bytes[off+1] == 'S') &&
        (bytes[off+2] == 'i' || bytes[off+2] == 'I') &&
        bytes[off+3] == ' ') {
        size_t eol = off;
        while (eol < bytes.size() && eol < off + 64 && bytes[eol] != '\r' && bytes[eol] != '\n') ++eol;
        std::string hdr(reinterpret_cast<const char*>(bytes.data() + off), eol - off);
        r.magic_summary = trim(hdr);
        if (hdr.find("0101txt") != std::string::npos || hdr.find("0101TXT") != std::string::npos) {
            r.format = SourceFormat::XSI_Text_11;
            r.major_version = 1; r.minor_version = 1;
            r.supported = true;
            r.detail = "Battlezone dotXSI 1.1 text format";
            return r;
        }
        if (hdr.find("0103txt") != std::string::npos || hdr.find("0103TXT") != std::string::npos) {
            r.format = SourceFormat::XSI_Text_13;
            r.major_version = 1; r.minor_version = 3;
            r.supported = true;
            r.detail = "Softimage dotXSI 1.3 text format";
            return r;
        }
        if (hdr.find("bin") != std::string::npos || hdr.find("BIN") != std::string::npos) {
            r.format = SourceFormat::Malformed;
            r.detail = "Binary dotXSI variant is unsupported: " + r.magic_summary;
            return r;
        }
        r.format = SourceFormat::Malformed;
        r.detail = "Unrecognized XSI header: " + r.magic_summary;
        return r;
    }

    // 2. Check Battlezone DOCB binary MSH
    if (std::memcmp(bytes.data(), "DOCB", 4) == 0) {
        if (bytes.size() < 52) {
            r.format = SourceFormat::Malformed;
            r.magic_summary = "DOCB (truncated)";
            r.detail = "DOCB header truncated (< 52 bytes)";
            return r;
        }
        uint32_t ver = 0, layout = 0, stamp = 0, bodylen = 0;
        std::memcpy(&ver, bytes.data() + 4, 4);
        std::memcpy(&layout, bytes.data() + 8, 4);
        std::memcpy(&stamp, bytes.data() + 44, 4);
        std::memcpy(&bodylen, bytes.data() + 48, 4);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "DOCB v%u.%u stamp=0x%08x body=%u", ver, layout, stamp, bodylen);
        r.magic_summary = buf;
        r.major_version = ver;
        r.minor_version = layout;
        if (ver != 1 || layout != 1) {
            r.format = SourceFormat::MSH_UnknownVariant_Unsupported;
            r.supported = false;
            r.detail = "Unsupported DOCB version/layout: " + r.magic_summary;
            return r;
        }
        if (bodylen > bytes.size() - 52) {
            r.format = SourceFormat::Malformed;
            r.supported = false;
            r.detail = "DOCB body length exceeds file size (" + std::to_string(bodylen) + " > " + std::to_string(bytes.size() - 52) + ")";
            return r;
        }
        // Distinguish BZCC (.material references + 0x9709513f) vs Classic BZ2 (.pic/.dxtbz2 inline textures)
        bool has_material_ext = false;
        const char* needle = ".material";
        for (size_t i = 52; i + 9 <= bytes.size(); ++i) {
            if (std::memcmp(bytes.data() + i, needle, 9) == 0) { has_material_ext = true; break; }
        }
        r.format = has_material_ext ? SourceFormat::MSH_DOCB_V1_BZCC : SourceFormat::MSH_DOCB_V1_BZ2;
        r.supported = true;
        r.detail = has_material_ext ? "Battlezone Combat Commander DOCB v1 baked MSH" : "Battlezone II DOCB v1 baked MSH";
        return r;
    }

    // 3. Explicit rejection of Star Wars Battlefront MSH2 ("HEDR" + "MSH2" chunk)
    if (std::memcmp(bytes.data(), "HEDR", 4) == 0 ||
        (bytes.size() >= 12 && std::memcmp(bytes.data() + 8, "MSH2", 4) == 0)) {
        r.format = SourceFormat::MSH_SWBF_MSH2_Unsupported;
        r.supported = false;
        r.magic_summary = "HEDR/MSH2 (Star Wars Battlefront)";
        r.detail = "Star Wars Battlefront MSH2 format is NOT Battlezone DOCB and is explicitly rejected";
        return r;
    }

    // 4. Unknown binary variant (e.g., zfoliag2.msh starting with 00 02 0b 03)
    char hex[32];
    std::snprintf(hex, sizeof(hex), "%02x %02x %02x %02x", bytes[0], bytes[1], bytes[2], bytes[3]);
    r.magic_summary = std::string("bytes[") + hex + "]";
    std::string ext = to_lower(utf8_from_path(path_hint.extension()));
    if (ext == ".msh") {
        r.format = SourceFormat::MSH_UnknownVariant_Unsupported;
        r.supported = false;
        r.detail = "Unrecognized non-DOCB .msh binary variant (" + r.magic_summary + "); refused to prevent corrupt export";
    } else {
        r.format = SourceFormat::Unknown;
        r.supported = false;
        r.detail = "Unrecognized file signature: " + r.magic_summary;
    }
    return r;
}

ProbeResult probe_file(const std::filesystem::path& path) {
    std::vector<uint8_t> bytes;
    std::string err;
    if (!read_file_bytes(path, bytes, err)) {
        ProbeResult r;
        r.format = SourceFormat::Malformed;
        r.supported = false;
        r.detail = err;
        return r;
    }
    return probe_bytes(bytes, path);
}

} // namespace bz
