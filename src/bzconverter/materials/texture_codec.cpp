#include "bzconverter/materials/texture_codec.hpp"
#include "bzconverter/core/types.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <zlib.h>

namespace bz {

namespace {

uint32_t rd_u32_le(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint16_t rd_u16_le(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t rd_u32_be(const uint8_t* p) { return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3]; }
uint16_t rd_u16_be(const uint8_t* p) { return uint16_t((uint16_t(p[0])<<8)|p[1]); }

void unpack_rgb565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint8_t r5 = (c >> 11) & 0x1F;
    uint8_t g6 = (c >> 5) & 0x3F;
    uint8_t b5 = c & 0x1F;
    r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
}

void decode_dxt_color_block(const uint8_t* blk, uint8_t out_rgba[16][4], bool allow_transparent_1bit) {
    uint16_t c0 = rd_u16_le(blk);
    uint16_t c1 = rd_u16_le(blk + 2);
    uint8_t pal[4][4];
    unpack_rgb565(c0, pal[0][0], pal[0][1], pal[0][2]); pal[0][3] = 255;
    unpack_rgb565(c1, pal[1][0], pal[1][1], pal[1][2]); pal[1][3] = 255;
    if (c0 > c1 || !allow_transparent_1bit) {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = static_cast<uint8_t>((2 * pal[0][i] + pal[1][i] + 1) / 3);
            pal[3][i] = static_cast<uint8_t>((pal[0][i] + 2 * pal[1][i] + 1) / 3);
        }
        pal[2][3] = 255; pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) pal[2][i] = static_cast<uint8_t>((pal[0][i] + pal[1][i]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    uint32_t bits = rd_u32_le(blk + 4);
    for (int i = 0; i < 16; ++i) {
        int code = (bits >> (2 * i)) & 3;
        std::memcpy(out_rgba[i], pal[code], 4);
    }
}

void decode_dxt5_alpha_block(const uint8_t* blk, uint8_t out_a[16]) {
    uint8_t a0 = blk[0], a1 = blk[1];
    uint8_t pal[8];
    pal[0] = a0; pal[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; ++i)
            pal[1 + i] = static_cast<uint8_t>(((7 - i) * a0 + i * a1 + 3) / 7);
    } else {
        for (int i = 1; i <= 4; ++i)
            pal[1 + i] = static_cast<uint8_t>(((5 - i) * a0 + i * a1 + 2) / 5);
        pal[6] = 0; pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= (uint64_t(blk[2 + i]) << (8 * i));
    for (int i = 0; i < 16; ++i) out_a[i] = pal[(bits >> (3 * i)) & 7];
}

bool decode_dxt_blocks(const uint8_t* src, size_t src_len, int w, int h, int dxt_type, DecodedImage& out, std::string& err) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) { err = "Invalid DXT dimensions"; return false; }
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;
    size_t bsize = (dxt_type == 1) ? 8 : 16;
    if (size_t(bw) * bh * bsize > src_len) { err = "Truncated DXT mip0 data"; return false; }

    out.width = w;
    out.height = h;
    out.rgba.assign(size_t(w) * h * 4, 255);
    out.has_non_opaque_alpha = false;

    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = src + (size_t(by) * bw + bx) * bsize;
            uint8_t px[16][4];
            if (dxt_type == 1) {
                decode_dxt_color_block(blk, px, true);
            } else if (dxt_type == 3) {
                decode_dxt_color_block(blk + 8, px, false);
                for (int i = 0; i < 16; ++i) {
                    uint8_t nib = (blk[i / 2] >> ((i & 1) * 4)) & 0x0F;
                    px[i][3] = static_cast<uint8_t>((nib << 4) | nib);
                }
            } else if (dxt_type == 5) {
                uint8_t alphas[16];
                decode_dxt5_alpha_block(blk, alphas);
                decode_dxt_color_block(blk + 8, px, false);
                for (int i = 0; i < 16; ++i) px[i][3] = alphas[i];
            } else {
                err = "Unsupported DXT type";
                return false;
            }
            for (int py = 0; py < 4; ++py) {
                int y = by * 4 + py;
                if (y >= h) continue;
                for (int px_i = 0; px_i < 4; ++px_i) {
                    int x = bx * 4 + px_i;
                    if (x >= w) continue;
                    const uint8_t* c = px[py * 4 + px_i];
                    uint8_t* dst = &out.rgba[(size_t(y) * w + x) * 4];
                    dst[0] = c[0]; dst[1] = c[1]; dst[2] = c[2]; dst[3] = c[3];
                    if (c[3] < 255) out.has_non_opaque_alpha = true;
                }
            }
        }
    }
    return true;
}

void decode_bc5s_signed_block(const uint8_t* blk, int8_t out_s[16]) {
    int8_t s0 = static_cast<int8_t>(blk[0]), s1 = static_cast<int8_t>(blk[1]);
    int8_t pal[8];
    pal[0] = s0; pal[1] = s1;
    if (s0 > s1) {
        for (int i = 1; i <= 6; ++i)
            pal[1 + i] = static_cast<int8_t>(((7 - i) * s0 + i * s1) / 7);
    } else {
        for (int i = 1; i <= 4; ++i)
            pal[1 + i] = static_cast<int8_t>(((5 - i) * s0 + i * s1) / 5);
        pal[6] = -128; pal[7] = 127;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= (uint64_t(blk[2 + i]) << (8 * i));
    for (int i = 0; i < 16; ++i) out_s[i] = pal[(bits >> (3 * i)) & 7];
}

bool decode_bc5_blocks(const uint8_t* src, size_t src_len, int w, int h, bool is_signed, DecodedImage& out, std::string& err) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) { err = "Invalid BC5 dimensions"; return false; }
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;
    size_t bsize = 16;
    if (size_t(bw) * bh * bsize > src_len) { err = "Truncated BC5 mip0 data"; return false; }

    out.width = w;
    out.height = h;
    out.rgba.resize(size_t(w) * h * 4);

    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = src + (size_t(by) * bw + bx) * bsize;
            float fx[16];
            float fy[16];

            if (is_signed) {
                int8_t sx[16], sy[16];
                decode_bc5s_signed_block(blk, sx);
                decode_bc5s_signed_block(blk + 8, sy);
                for (int i = 0; i < 16; ++i) {
                    fx[i] = std::clamp(float(sx[i]) / 127.0f, -1.0f, 1.0f);
                    fy[i] = std::clamp(float(sy[i]) / 127.0f, -1.0f, 1.0f);
                }
            } else {
                uint8_t ux[16], uy[16];
                decode_dxt5_alpha_block(blk, ux);
                decode_dxt5_alpha_block(blk + 8, uy);
                for (int i = 0; i < 16; ++i) {
                    fx[i] = (float(ux[i]) / 255.0f) * 2.0f - 1.0f;
                    fy[i] = (float(uy[i]) / 255.0f) * 2.0f - 1.0f;
                }
            }

            for (int py = 0; py < 4; ++py) {
                int y = by * 4 + py;
                if (y >= h) continue;
                for (int px_i = 0; px_i < 4; ++px_i) {
                    int x = bx * 4 + px_i;
                    if (x >= w) continue;
                    int i = py * 4 + px_i;
                    float nx = fx[i];
                    float ny = fy[i];
                    float nz_sq = 1.0f - nx * nx - ny * ny;
                    float nz = nz_sq > 0.0f ? std::sqrt(nz_sq) : 0.0f;

                    uint8_t r = static_cast<uint8_t>(std::clamp((nx * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
                    uint8_t g = static_cast<uint8_t>(std::clamp((ny * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
                    uint8_t b = static_cast<uint8_t>(std::clamp((nz * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));

                    size_t dst_off = (size_t(y) * w + x) * 4;
                    out.rgba[dst_off + 0] = r;
                    out.rgba[dst_off + 1] = g;
                    out.rgba[dst_off + 2] = b;
                    out.rgba[dst_off + 3] = 255;
                }
            }
        }
    }
    return true;
}

bool decode_dxtbz2(const std::vector<uint8_t>& bytes, DecodedImage& out, std::string& err) {
    if (bytes.size() < 28 || std::memcmp(bytes.data(), "!HSG", 4) != 0) {
        err = "Invalid DXTBZ2 header";
        return false;
    }
    uint32_t fmt = rd_u32_le(bytes.data() + 4);
    uint32_t mips = rd_u32_le(bytes.data() + 12);
    uint32_t w = rd_u32_le(bytes.data() + 16);
    uint32_t h = rd_u32_le(bytes.data() + 20);
    uint32_t mip0_sz = rd_u32_le(bytes.data() + 24);
    (void)mips;
    if (28 + size_t(mip0_sz) > bytes.size()) {
        err = "Truncated DXTBZ2 mip0 payload";
        return false;
    }
    int dxt = 0;
    if (fmt == 1) dxt = 1;
    else if (fmt == 3) dxt = 3;
    else if (fmt == 5) dxt = 5;
    else { err = "Unsupported DXTBZ2 format code " + std::to_string(fmt); return false; }
    out.source_format = "DXTBZ2_DXT" + std::to_string(dxt);
    return decode_dxt_blocks(bytes.data() + 28, mip0_sz, static_cast<int>(w), static_cast<int>(h), dxt, out, err);
}

bool decode_dds(const std::vector<uint8_t>& bytes, DecodedImage& out, std::string& err) {
    if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4) != 0) {
        err = "Invalid DDS magic";
        return false;
    }
    uint32_t h = rd_u32_le(bytes.data() + 12);
    uint32_t w = rd_u32_le(bytes.data() + 16);
    uint32_t pf_flags = rd_u32_le(bytes.data() + 80);
    const uint8_t* fourcc = bytes.data() + 84;
    uint32_t rgb_bits = rd_u32_le(bytes.data() + 88);
    uint32_t r_mask = rd_u32_le(bytes.data() + 92);
    uint32_t g_mask = rd_u32_le(bytes.data() + 96);
    uint32_t b_mask = rd_u32_le(bytes.data() + 100);
    uint32_t a_mask = rd_u32_le(bytes.data() + 104);

    if (w == 0 || h == 0 || w > 16384 || h > 16384) { err = "Invalid DDS dimensions"; return false; }

    size_t data_off = 128;
    if ((pf_flags & 0x4u) != 0) {
        // FourCC compressed
        if (std::memcmp(fourcc, "DXT1", 4) == 0) {
            out.source_format = "DDS_DXT1";
            return decode_dxt_blocks(bytes.data() + data_off, bytes.size() - data_off, int(w), int(h), 1, out, err);
        }
        if (std::memcmp(fourcc, "DXT3", 4) == 0) {
            out.source_format = "DDS_DXT3";
            return decode_dxt_blocks(bytes.data() + data_off, bytes.size() - data_off, int(w), int(h), 3, out, err);
        }
        if (std::memcmp(fourcc, "DXT5", 4) == 0) {
            out.source_format = "DDS_DXT5";
            return decode_dxt_blocks(bytes.data() + data_off, bytes.size() - data_off, int(w), int(h), 5, out, err);
        }
        if (std::memcmp(fourcc, "ATI2", 4) == 0 || std::memcmp(fourcc, "BC5U", 4) == 0) {
            out.source_format = "DDS_BC5U";
            return decode_bc5_blocks(bytes.data() + data_off, bytes.size() - data_off, int(w), int(h), false, out, err);
        }
        if (std::memcmp(fourcc, "BC5S", 4) == 0) {
            out.source_format = "DDS_BC5S";
            return decode_bc5_blocks(bytes.data() + data_off, bytes.size() - data_off, int(w), int(h), true, out, err);
        }
        if (std::memcmp(fourcc, "DX10", 4) == 0 && bytes.size() >= 148) {
            uint32_t dxgi = rd_u32_le(bytes.data() + 128);
            data_off = 148;
            if (dxgi == 70 || dxgi == 71 || dxgi == 72) {
                out.source_format = "DDS_BC1";
                return decode_dxt_blocks(bytes.data() + data_off, bytes.size() - data_off, int(w), int(h), 1, out, err);
            }
            if (dxgi == 73 || dxgi == 74 || dxgi == 75) {
                out.source_format = "DDS_BC2";
                return decode_dxt_blocks(bytes.data() + data_off, bytes.size() - data_off, int(w), int(h), 3, out, err);
            }
            if (dxgi == 76 || dxgi == 77 || dxgi == 78) {
                out.source_format = "DDS_BC3";
                return decode_dxt_blocks(bytes.data() + data_off, bytes.size() - data_off, int(w), int(h), 5, out, err);
            }
            if (dxgi == 82 || dxgi == 83 || dxgi == 84) {
                out.source_format = (dxgi == 84) ? "DDS_BC5S" : "DDS_BC5U";
                return decode_bc5_blocks(bytes.data() + data_off, bytes.size() - data_off, int(w), int(h), dxgi == 84, out, err);
            }
            err = "Unsupported DX10 DDS format DXGI=" + std::to_string(dxgi);
            return false;
        }
        err = "Unsupported DDS FourCC: " + std::string(reinterpret_cast<const char*>(fourcc), 4);
        return false;
    }

    // Uncompressed RGB/RGBA DDS
    if (rgb_bits != 24 && rgb_bits != 32) {
        err = "Unsupported uncompressed DDS bit depth: " + std::to_string(rgb_bits);
        return false;
    }
    size_t bpp = rgb_bits / 8;
    if (data_off + size_t(w) * h * bpp > bytes.size()) { err = "Truncated uncompressed DDS"; return false; }
    auto extract = [](uint32_t px, uint32_t mask) -> uint8_t {
        if (!mask) return 255;
        int shift = 0;
        uint32_t m = mask;
        while ((m & 1u) == 0) { m >>= 1; ++shift; }
        uint32_t v = (px & mask) >> shift;
        return static_cast<uint8_t>((v * 255u) / std::max(1u, m));
    };
    out.width = int(w); out.height = int(h);
    out.rgba.resize(size_t(w) * h * 4);
    out.has_non_opaque_alpha = false;
    out.source_format = "DDS_UNCOMPRESSED";
    const uint8_t* p = bytes.data() + data_off;
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        uint32_t px = 0;
        std::memcpy(&px, p + i * bpp, bpp);
        out.rgba[i*4 + 0] = extract(px, r_mask);
        out.rgba[i*4 + 1] = extract(px, g_mask);
        out.rgba[i*4 + 2] = extract(px, b_mask);
        uint8_t a = a_mask ? extract(px, a_mask) : 255;
        out.rgba[i*4 + 3] = a;
        if (a < 255) out.has_non_opaque_alpha = true;
    }
    return true;
}

bool decode_softimage_pic(const std::vector<uint8_t>& bytes, DecodedImage& out, std::string& err) {
    if (bytes.size() < 108 || rd_u32_be(bytes.data()) != 0x5380F634u || std::memcmp(bytes.data() + 88, "PICT", 4) != 0) {
        err = "Invalid Softimage PIC header";
        return false;
    }
    int w = rd_u16_be(bytes.data() + 92);
    int h = rd_u16_be(bytes.data() + 94);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) { err = "Invalid PIC dimensions"; return false; }

    struct Channel { uint8_t size, type, mask; };
    std::vector<Channel> channels;
    size_t at = 104;
    while (at + 4 <= bytes.size()) {
        uint8_t chained = bytes[at], sz = bytes[at+1], ctype = bytes[at+2], mask = bytes[at+3];
        at += 4;
        if (sz != 8) { err = "Unsupported Softimage PIC bit depth: " + std::to_string(sz); return false; }
        channels.push_back({sz, ctype, mask});
        if (!chained) break;
    }
    if (channels.empty()) { err = "PIC has no channel packets"; return false; }

    out.width = w; out.height = h;
    out.rgba.assign(size_t(w) * h * 4, 255);
    out.has_non_opaque_alpha = false;
    out.source_format = "SOFTIMAGE_PIC";

    auto write_ch = [&](int x, int y, uint8_t mask, const uint8_t* vals) {
        uint8_t* dst = &out.rgba[(size_t(y) * w + x) * 4];
        int vi = 0;
        if (mask & 0x80) dst[0] = vals[vi++];
        if (mask & 0x40) dst[1] = vals[vi++];
        if (mask & 0x20) dst[2] = vals[vi++];
        if (mask & 0x10) { dst[3] = vals[vi++]; if (dst[3] < 255) out.has_non_opaque_alpha = true; }
    };

    for (int y = 0; y < h; ++y) {
        for (const auto& ch : channels) {
            int ncomp = ((ch.mask & 0x80) ? 1 : 0) + ((ch.mask & 0x40) ? 1 : 0) +
                        ((ch.mask & 0x20) ? 1 : 0) + ((ch.mask & 0x10) ? 1 : 0);
            if (ncomp == 0) continue;
            if (ch.type == 0) {
                // Uncompressed
                if (at + size_t(w) * ncomp > bytes.size()) { err = "Truncated uncompressed PIC"; return false; }
                for (int x = 0; x < w; ++x) {
                    write_ch(x, y, ch.mask, &bytes[at]);
                    at += ncomp;
                }
            } else if (ch.type == 2) {
                // Mixed RLE (Softimage standard)
                int x = 0;
                while (x < w) {
                    if (at >= bytes.size()) { err = "Truncated RLE PIC"; return false; }
                    uint8_t cnt = bytes[at++];
                    if (cnt < 128) {
                        int run = cnt + 1;
                        if (at + size_t(run) * ncomp > bytes.size()) { err = "Truncated PIC raw run"; return false; }
                        for (int r = 0; r < run && x < w; ++r, ++x) {
                            write_ch(x, y, ch.mask, &bytes[at]);
                            at += ncomp;
                        }
                    } else {
                        int run = (cnt == 128) ? (at + 2 <= bytes.size() ? (at += 2, rd_u16_be(&bytes[at - 2])) : 0) : (cnt - 127);
                        if (run <= 0 || at + ncomp > bytes.size()) { err = "Truncated PIC repeat run"; return false; }
                        const uint8_t* val = &bytes[at];
                        at += ncomp;
                        for (int r = 0; r < run && x < w; ++r, ++x) write_ch(x, y, ch.mask, val);
                    }
                }
            } else {
                err = "Unsupported PIC compression type " + std::to_string(ch.type);
                return false;
            }
        }
    }
    return true;
}

bool decode_tga(const std::vector<uint8_t>& bytes, DecodedImage& out, std::string& err) {
    if (bytes.size() < 18) { err = "TGA too small"; return false; }
    uint8_t id_len = bytes[0];
    uint8_t cmap_type = bytes[1];
    uint8_t img_type = bytes[2];
    int w = rd_u16_le(bytes.data() + 12);
    int h = rd_u16_le(bytes.data() + 14);
    uint8_t bpp = bytes[16];
    uint8_t desc = bytes[17];
    if (cmap_type != 0 || (img_type != 2 && img_type != 10) || (bpp != 24 && bpp != 32) || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        err = "Unsupported TGA format";
        return false;
    }
    size_t at = 18 + id_len;
    int step = bpp / 8;
    bool top_down = (desc & 0x20) != 0;
    out.width = w; out.height = h;
    out.rgba.resize(size_t(w) * h * 4);
    out.has_non_opaque_alpha = false;
    out.source_format = "TGA";

    auto put_px = [&](size_t idx, const uint8_t* src) {
        int x = static_cast<int>(idx % w);
        int y_raw = static_cast<int>(idx / w);
        int y = top_down ? y_raw : (h - 1 - y_raw);
        uint8_t* dst = &out.rgba[(size_t(y) * w + x) * 4];
        dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0];
        dst[3] = (step == 4) ? src[3] : 255;
        if (dst[3] < 255) out.has_non_opaque_alpha = true;
    };

    size_t total = size_t(w) * h;
    if (img_type == 2) {
        if (at + total * step > bytes.size()) { err = "Truncated TGA"; return false; }
        for (size_t i = 0; i < total; ++i) put_px(i, &bytes[at + i * step]);
    } else {
        size_t i = 0;
        while (i < total) {
            if (at >= bytes.size()) { err = "Truncated RLE TGA"; return false; }
            uint8_t hdr = bytes[at++];
            int cnt = (hdr & 0x7F) + 1;
            if (hdr & 0x80) {
                if (at + step > bytes.size()) { err = "Truncated RLE TGA packet"; return false; }
                const uint8_t* px = &bytes[at]; at += step;
                for (int r = 0; r < cnt && i < total; ++r, ++i) put_px(i, px);
            } else {
                if (at + size_t(cnt) * step > bytes.size()) { err = "Truncated raw TGA packet"; return false; }
                for (int r = 0; r < cnt && i < total; ++r, ++i, at += step) put_px(i, &bytes[at]);
            }
        }
    }
    return true;
}

bool decode_bmp(const std::vector<uint8_t>& bytes, DecodedImage& out, std::string& err) {
    if (bytes.size() < 54 || bytes[0] != 'B' || bytes[1] != 'M') { err = "Invalid BMP"; return false; }
    uint32_t data_off = rd_u32_le(bytes.data() + 10);
    int32_t w = static_cast<int32_t>(rd_u32_le(bytes.data() + 18));
    int32_t h_raw = static_cast<int32_t>(rd_u32_le(bytes.data() + 22));
    uint16_t bpp = rd_u16_le(bytes.data() + 28);
    uint32_t comp = rd_u32_le(bytes.data() + 30);
    int h = std::abs(h_raw);
    bool top_down = (h_raw < 0);
    if (comp != 0 || (bpp != 24 && bpp != 32) || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        err = "Unsupported BMP format";
        return false;
    }
    size_t row_stride = ((size_t(w) * (bpp / 8) + 3) / 4) * 4;
    if (data_off + row_stride * h > bytes.size()) { err = "Truncated BMP"; return false; }
    out.width = w; out.height = h;
    out.rgba.resize(size_t(w) * h * 4);
    out.has_non_opaque_alpha = false;
    out.source_format = "BMP";
    int step = bpp / 8;
    for (int y_raw = 0; y_raw < h; ++y_raw) {
        int y = top_down ? y_raw : (h - 1 - y_raw);
        const uint8_t* src_row = bytes.data() + data_off + size_t(y_raw) * row_stride;
        for (int x = 0; x < w; ++x) {
            uint8_t* dst = &out.rgba[(size_t(y) * w + x) * 4];
            dst[0] = src_row[x * step + 2];
            dst[1] = src_row[x * step + 1];
            dst[2] = src_row[x * step + 0];
            dst[3] = (step == 4) ? src_row[x * step + 3] : 255;
            if (dst[3] < 255) out.has_non_opaque_alpha = true;
        }
    }
    return true;
}

void push_be32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void push_png_chunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t len) {
    push_be32(out, static_cast<uint32_t>(len));
    size_t type_pos = out.size();
    out.insert(out.end(), type, type + 4);
    if (len > 0) out.insert(out.end(), data, data + len);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, out.data() + type_pos, static_cast<uInt>(len + 4));
    push_be32(out, static_cast<uint32_t>(crc));
}

} // namespace

bool decode_image_bytes(const std::vector<uint8_t>& bytes, const std::string& ext_hint,
                        DecodedImage& out, std::string& err) {
    if (bytes.size() < 4) { err = "Image file too small"; return false; }
    if (std::memcmp(bytes.data(), "!HSG", 4) == 0) return decode_dxtbz2(bytes, out, err);
    if (std::memcmp(bytes.data(), "DDS ", 4) == 0) return decode_dds(bytes, out, err);
    if (rd_u32_be(bytes.data()) == 0x5380F634u) return decode_softimage_pic(bytes, out, err);
    if (bytes[0] == 'B' && bytes[1] == 'M') return decode_bmp(bytes, out, err);
    std::string lo = to_lower(ext_hint);
    if (lo == ".tga") return decode_tga(bytes, out, err);
    err = "Unsupported or unrecognized image format (" + ext_hint + ")";
    return false;
}

bool decode_image_file(const std::filesystem::path& path, DecodedImage& out, std::string& err) {
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(path, bytes, err)) return false;
    return decode_image_bytes(bytes, utf8_from_path(path.extension()), out, err);
}

bool encode_png_bytes(int width, int height, const uint8_t* rgba, std::vector<uint8_t>& out_png, std::string& err) {
    if (width <= 0 || height <= 0 || !rgba) { err = "Invalid RGBA image for PNG encode"; return false; }
    size_t stride = size_t(width) * 4;
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (stride + 1));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0); // filter None
        const uint8_t* row = rgba + size_t(y) * stride;
        raw.insert(raw.end(), row, row + stride);
    }
    uLongf comp_cap = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> comp(comp_cap);
    int zrc = compress2(comp.data(), &comp_cap, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_SPEED);
    if (zrc != Z_OK) { err = "zlib compress2 failed (" + std::to_string(zrc) + ")"; return false; }
    comp.resize(comp_cap);

    out_png.clear();
    static constexpr uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out_png.insert(out_png.end(), sig, sig + 8);
    uint8_t ihdr[13];
    ihdr[0]=(width>>24)&255; ihdr[1]=(width>>16)&255; ihdr[2]=(width>>8)&255; ihdr[3]=width&255;
    ihdr[4]=(height>>24)&255; ihdr[5]=(height>>16)&255; ihdr[6]=(height>>8)&255; ihdr[7]=height&255;
    ihdr[8]=8; ihdr[9]=6; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0; // 8-bit RGBA
    push_png_chunk(out_png, "IHDR", ihdr, 13);
    push_png_chunk(out_png, "IDAT", comp.data(), comp.size());
    push_png_chunk(out_png, "IEND", nullptr, 0);
    return true;
}

bool write_png_file(const std::filesystem::path& path, int width, int height, const uint8_t* rgba, std::string& err) {
    std::vector<uint8_t> png;
    if (!encode_png_bytes(width, height, rgba, png, err)) return false;
    return write_file_atomic(path, png.data(), png.size(), err);
}

bool stage_texture_as_png(const std::filesystem::path& src_path,
                          const std::filesystem::path& cache_dir,
                          std::filesystem::path& out_png_path,
                          bool& out_has_alpha,
                          std::string& err) {
    std::vector<uint8_t> bytes;
    if (!read_file_bytes(src_path, bytes, err)) return false;
    if (bytes.size() < 8) { err = "Texture file too small: " + utf8_from_path(src_path); return false; }

    // Already valid PNG? Keep directly (or copy into cache)
    static constexpr uint8_t png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (std::memcmp(bytes.data(), png_sig, 8) == 0) {
        out_png_path = src_path;
        // Check color type in IHDR byte 25 (4=GA, 6=RGBA)
        out_has_alpha = (bytes.size() > 25 && (bytes[25] == 4 || bytes[25] == 6));
        return true;
    }
    // Already valid JPEG? Pass through (no alpha)
    if (bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        out_png_path = src_path;
        out_has_alpha = false;
        return true;
    }

    // Decode Battlezone / DDS / PIC / DXTBZ2 / TGA / BMP to lossless PNG
    DecodedImage img;
    if (!decode_image_bytes(bytes, utf8_from_path(src_path.extension()), img, err)) return false;
    out_has_alpha = img.has_non_opaque_alpha;

    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    std::string stem = utf8_from_path(src_path.stem());
    // Include short hash of full path to avoid collision between mods
    std::string sp = to_lower(utf8_from_path(src_path));
    uint32_t h = 2166136261u;
    for (unsigned char c : sp) { h ^= c; h *= 16777619u; }
    char hbuf[16];
    std::snprintf(hbuf, sizeof(hbuf), "_%08x", h);
    out_png_path = cache_dir / path_from_utf8(stem + hbuf + ".png");

    // Reuse cached PNG if newer than source
    if (std::filesystem::exists(out_png_path, ec)) {
        auto t_src = std::filesystem::last_write_time(src_path, ec);
        auto t_dst = std::filesystem::last_write_time(out_png_path, ec);
        if (!ec && t_dst >= t_src && std::filesystem::file_size(out_png_path, ec) > 32) {
            return true;
        }
    }
    return write_png_file(out_png_path, img.width, img.height, img.rgba.data(), err);
}

} // namespace bz
