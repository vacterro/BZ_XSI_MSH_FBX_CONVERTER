#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bz {

struct Vec2 {
    float x = 0.0f, y = 0.0f;
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 normalized() const {
        float l = length();
        if (l > 1e-20f) return {x / l, y / l, z / l};
        return {0.0f, 1.0f, 0.0f};
    }
};

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};

struct Quat {
    // Standard right-handed column-vector quaternion (w, x, y, z)
    float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
    Quat normalized() const {
        float l = std::sqrt(w * w + x * x + y * y + z * z);
        if (l > 1e-20f) return {w / l, x / l, y / l, z / l};
        return {1.0f, 0.0f, 0.0f, 0.0f};
    }
    Quat conjugate() const { return {w, -x, -y, -z}; }
};

// Row-major 4x4 matrix stored as row-vector transform:
// v_out = v_in * M, translation in row 3: m[12], m[13], m[14], m[15]=1.
// This matches both Softimage dotXSI and Battlezone DOCB native layout.
struct Mat4 {
    std::array<float, 16> m = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };

    static Mat4 identity() { return Mat4{}; }

    float& at(int r, int c) { return m[r * 4 + c]; }
    float at(int r, int c) const { return m[r * 4 + c]; }

    // Row-vector composition: v * (A * B) = (v * A) * B
    Mat4 operator*(const Mat4& b) const {
        Mat4 out;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float s = 0.0f;
                for (int k = 0; k < 4; ++k) s += m[r * 4 + k] * b.m[k * 4 + c];
                out.m[r * 4 + c] = s;
            }
        }
        return out;
    }

    Mat4 transposed() const {
        Mat4 out;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out.m[r * 4 + c] = m[c * 4 + r];
        return out;
    }

    Vec3 transform_point(const Vec3& p) const {
        return {
            p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12],
            p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
            p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14]
        };
    }

    Vec3 transform_vector(const Vec3& v) const {
        return {
            v.x * m[0] + v.y * m[4] + v.z * m[8],
            v.x * m[1] + v.y * m[5] + v.z * m[9],
            v.x * m[2] + v.y * m[6] + v.z * m[10]
        };
    }

    float det3x3() const {
        return m[0] * (m[5] * m[10] - m[6] * m[9])
             - m[1] * (m[4] * m[10] - m[6] * m[8])
             + m[2] * (m[4] * m[9]  - m[5] * m[8]);
    }

    bool invert_affine(Mat4& out) const {
        float a = m[0], b = m[1], c = m[2];
        float d = m[4], e = m[5], f = m[6];
        float g = m[8], h = m[9], i = m[10];
        float A = e * i - f * h;
        float B = -(d * i - f * g);
        float C = d * h - e * g;
        float det = a * A + b * B + c * C;
        if (!(std::fabs(det) > 1e-12f)) return false;
        float inv_det = 1.0f / det;
        float inv3[9] = {
            A * inv_det, -(b * i - c * h) * inv_det,  (b * f - c * e) * inv_det,
            B * inv_det,  (a * i - c * g) * inv_det, -(a * f - c * d) * inv_det,
            C * inv_det, -(a * h - b * g) * inv_det,  (a * e - b * d) * inv_det
        };
        for (int r = 0; r < 3; ++r) {
            for (int k = 0; k < 3; ++k) out.m[r * 4 + k] = inv3[r * 3 + k];
            out.m[r * 4 + 3] = 0.0f;
        }
        for (int k = 0; k < 3; ++k)
            out.m[12 + k] = -(m[12] * inv3[k] + m[13] * inv3[3 + k] + m[14] * inv3[6 + k]);
        out.m[15] = 1.0f;
        return true;
    }

    // Build row-vector transform matrix from BZ/XSI quaternion (where upper 3x3 == R_col(q)) and translation
    static Mat4 from_bz_trs(const Vec3& t, const Quat& bz_q, const Vec3& s = {1, 1, 1}) {
        Quat q = bz_q.normalized();
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        // R_col(q) matches DOCB/XSI row-vector 3x3 (verified err=0.000000 on ivcons_startup.msh)
        Mat4 out;
        out.m[0] = (1.0f - 2.0f * (yy + zz)) * s.x;
        out.m[1] = (2.0f * (xy - wz)) * s.x;
        out.m[2] = (2.0f * (xz + wy)) * s.x;
        out.m[3] = 0.0f;
        out.m[4] = (2.0f * (xy + wz)) * s.y;
        out.m[5] = (1.0f - 2.0f * (xx + zz)) * s.y;
        out.m[6] = (2.0f * (yz - wx)) * s.y;
        out.m[7] = 0.0f;
        out.m[8] = (2.0f * (xz - wy)) * s.z;
        out.m[9] = (2.0f * (yz + wx)) * s.z;
        out.m[10] = (1.0f - 2.0f * (xx + yy)) * s.z;
        out.m[11] = 0.0f;
        out.m[12] = t.x;
        out.m[13] = t.y;
        out.m[14] = t.z;
        out.m[15] = 1.0f;
        return out;
    }

    // Decompose affine row-vector matrix into translation, standard column-vector quaternion, and scale
    void decompose_column_trs(Vec3& out_t, Quat& out_col_q, Vec3& out_s) const {
        out_t = {m[12], m[13], m[14]};
        // Rows 0, 1, 2 of row-vector M are transformed X, Y, Z basis vectors
        Vec3 rx = {m[0], m[1], m[2]};
        Vec3 ry = {m[4], m[5], m[6]};
        Vec3 rz = {m[8], m[9], m[10]};
        out_s.x = rx.length();
        out_s.y = ry.length();
        out_s.z = rz.length();
        if (det3x3() < 0.0f) out_s.x = -out_s.x;

        // Column-vector rotation matrix R_col = transpose(R_row_normalized)
        // So column 0 of R_col is rx/sx, column 1 is ry/sy, column 2 is rz/sz!
        Vec3 c0 = (std::fabs(out_s.x) > 1e-12f) ? (rx * (1.0f / out_s.x)) : Vec3{1, 0, 0};
        Vec3 c1 = (std::fabs(out_s.y) > 1e-12f) ? (ry * (1.0f / out_s.y)) : Vec3{0, 1, 0};
        Vec3 c2 = (std::fabs(out_s.z) > 1e-12f) ? (rz * (1.0f / out_s.z)) : Vec3{0, 0, 1};
        // R[row][col]:
        float r00 = c0.x, r01 = c1.x, r02 = c2.x;
        float r10 = c0.y, r11 = c1.y, r12 = c2.y;
        float r20 = c0.z, r21 = c1.z, r22 = c2.z;

        float tr = r00 + r11 + r22;
        if (tr > 0.0f) {
            float S = std::sqrt(tr + 1.0f) * 2.0f;
            out_col_q = {0.25f * S, (r21 - r12) / S, (r02 - r20) / S, (r10 - r01) / S};
        } else if (r00 > r11 && r00 > r22) {
            float S = std::sqrt(std::max(0.0f, 1.0f + r00 - r11 - r22)) * 2.0f;
            if (S < 1e-12f) out_col_q = {1, 0, 0, 0};
            else out_col_q = {(r21 - r12) / S, 0.25f * S, (r01 + r10) / S, (r02 + r20) / S};
        } else if (r11 > r22) {
            float S = std::sqrt(std::max(0.0f, 1.0f + r11 - r00 - r22)) * 2.0f;
            if (S < 1e-12f) out_col_q = {1, 0, 0, 0};
            else out_col_q = {(r02 - r20) / S, (r01 + r10) / S, 0.25f * S, (r12 + r21) / S};
        } else {
            float S = std::sqrt(std::max(0.0f, 1.0f + r22 - r00 - r11)) * 2.0f;
            if (S < 1e-12f) out_col_q = {1, 0, 0, 0};
            else out_col_q = {(r10 - r01) / S, (r02 + r20) / S, (r12 + r21) / S, 0.25f * S};
        }
        out_col_q = out_col_q.normalized();
    }
};

struct BBox {
    Vec3 lo = {1e30f, 1e30f, 1e30f};
    Vec3 hi = {-1e30f, -1e30f, -1e30f};
    void expand(const Vec3& p) {
        lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
        hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
    }
    void expand(const BBox& b) {
        if (!b.empty()) { expand(b.lo); expand(b.hi); }
    }
    bool empty() const { return lo.x > hi.x; }
    Vec3 center() const { return empty() ? Vec3{} : Vec3{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f}; }
    Vec3 extent() const { return empty() ? Vec3{} : Vec3{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}; }
    float diagonal() const { return extent().length(); }
};

enum class DiagSeverity { Info, Warning, Error };

struct Diagnostic {
    DiagSeverity severity = DiagSeverity::Info;
    std::string code;
    std::string message;
};

enum class JobStatus { Ready, Running, Success, Partial, Failed, Cancelled };

// Honest result classes. Never collapse these into a green SUCCESS.
enum class ResultClass {
    VerifiedSuccess,
    SuccessUnverified,
    Partial,
    NeedsCompanionAsset,
    Unsupported,
    Failed,
    Cancelled
};

inline const char* result_class_to_string(ResultClass c) {
    switch (c) {
        case ResultClass::VerifiedSuccess: return "VERIFIED_SUCCESS";
        case ResultClass::SuccessUnverified: return "SUCCESS_UNVERIFIED";
        case ResultClass::Partial: return "PARTIAL";
        case ResultClass::NeedsCompanionAsset: return "NEEDS_COMPANION_ASSET";
        case ResultClass::Unsupported: return "UNSUPPORTED";
        case ResultClass::Failed: return "FAILED";
        case ResultClass::Cancelled: return "CANCELLED";
    }
    return "FAILED";
}

// Centralized result-class policy for diagnostics. A diagnostic that reports
// real fidelity loss must move the result out of SUCCESS programmatically;
// log text or severity alone is never a status signal.
struct DiagnosticPolicy {
    const char* code;
    ResultClass min_result_class;
    const char* feature;
    const char* consequence;
};

inline const DiagnosticPolicy* diagnostic_policy(const Diagnostic& d) {
    static const DiagnosticPolicy policies[] = {
        {"OBJ_FEATURE_LOSS", ResultClass::Partial, "skeleton/animation",
         "OBJ cannot carry bones or animation; result is explicitly PARTIAL"},
        {"XSI_UNSUPPORTED_ANIM_PARAM_KEY", ResultClass::Partial, "animation",
         "unsupported animation parameter key was dropped; result is PARTIAL"},
        {"GLB_INFLUENCE_LIMIT_LOSS", ResultClass::Partial, "skin weights",
         "compatibility profile truncated real influences; result is PARTIAL"},
        {"OBJ_TEXTURE_STAGE_FAILED", ResultClass::Partial, "textures",
         "a requested texture could not be staged with the OBJ bundle; result is PARTIAL"},
        {"GLTF_TEXTURE_STAGE_FAILED", ResultClass::Partial, "textures",
         "a requested texture could not be staged with the glTF bundle; result is PARTIAL"},
        {"GLTF_TEXTURE_MISSING", ResultClass::Partial, "textures",
         "a requested material texture file is missing; result is PARTIAL"},
        {"GLTF_TEXTURE_INVALID", ResultClass::Partial, "textures",
         "a requested material texture is corrupt or unsupported in glTF 2.0; result is PARTIAL"},
        {"GLB_SKIN_DATA_INCOMPLETE", ResultClass::Partial, "skinning",
         "skinned mesh data was incomplete or invalid; skinning dropped and result is PARTIAL"},
        {"GLB_SKIN_WEIGHTS_EMPTY", ResultClass::Partial, "skin weights",
         "skinned mesh had only zero weights; skinning dropped and result is PARTIAL"},
        {"GLB_MODEL_SPACE_INSTANCED", ResultClass::Partial, "instancing",
         "shared model-space mesh exported as per-instance copies; result is PARTIAL"},
        {"GLB_MODEL_SPACE_UNREFERENCED", ResultClass::Partial, "hierarchy",
         "model-space mesh had no referencing node; result is PARTIAL"},
    };
    for (const auto& p : policies) {
        if (d.code == p.code) return &p;
    }
    return nullptr;
}

inline bool diagnostic_forces_partial(const Diagnostic& d) {
    const DiagnosticPolicy* p = diagnostic_policy(d);
    return p && p->min_result_class == ResultClass::Partial;
}

inline const char* status_to_string(JobStatus s) {
    switch (s) {
        case JobStatus::Ready: return "READY";
        case JobStatus::Running: return "RUNNING";
        case JobStatus::Success: return "SUCCESS";
        case JobStatus::Partial: return "PARTIAL";
        case JobStatus::Failed: return "FAILED";
        case JobStatus::Cancelled: return "CANCELLED";
    }
    return "UNKNOWN";
}

inline std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

inline std::string utf8_from_path(const std::filesystem::path& p) {
    auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

inline std::filesystem::path path_from_utf8(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

inline bool read_file_bytes(const std::filesystem::path& path, std::vector<uint8_t>& out, std::string& err) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { err = "Failed to open file: " + utf8_from_path(path); return false; }
    auto sz = in.tellg();
    if (sz < 0) { err = "Failed to query size: " + utf8_from_path(path); return false; }
    out.resize(static_cast<size_t>(sz));
    in.seekg(0, std::ios::beg);
    if (sz > 0 && !in.read(reinterpret_cast<char*>(out.data()), sz)) {
        err = "Failed to read file bytes: " + utf8_from_path(path);
        return false;
    }
    return true;
}

inline bool write_file_atomic(const std::filesystem::path& target, const void* data, size_t size, std::string& err) {
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            err = "Cannot create output directory " + utf8_from_path(target.parent_path()) + ": " + ec.message();
            return false;
        }
        if (!std::filesystem::is_directory(target.parent_path(), ec)) {
            err = "Output parent is not a directory: " + utf8_from_path(target.parent_path());
            return false;
        }
    }

    static std::atomic<uint64_t> s_counter{0};
    uint64_t seq = ++s_counter;
    auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    auto uniq = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetCurrentThreadId()) +
                L"_" + std::to_wstring(now_ns) + L"_" + std::to_wstring(seq);
#else
    auto uniq = std::to_wstring(seq) + L"_" + std::to_wstring(now_ns);
#endif
    std::filesystem::path tmp = target;
    tmp += L".tmp_" + uniq + L".bzconv";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { err = "Cannot create temp output: " + utf8_from_path(tmp); return false; }
        if (size > 0) out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        out.flush();
        if (!out.good()) {
            out.close();
            std::filesystem::remove(tmp, ec);
            err = "Write failed for temp output: " + utf8_from_path(tmp);
            return false;
        }
    }

#ifdef _WIN32
    if (std::filesystem::exists(target, ec)) {
        std::filesystem::path bak = target;
        bak += L".bak_" + uniq + L".bzconv";
        std::error_code bak_ec;
        if (std::filesystem::exists(bak, bak_ec)) {
            std::filesystem::remove(tmp, ec);
            err = "Recovery backup path already occupied: " + utf8_from_path(bak);
            return false;
        }

        if (ReplaceFileW(target.c_str(), tmp.c_str(), bak.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            std::filesystem::remove(bak, ec);
            return true;
        }
        DWORD replace_err = GetLastError();
        // ReplaceFileW may have consumed the temp file even on failure. Never
        // claim success unless the target really contains the new bytes.
        if (!std::filesystem::exists(tmp, ec)) {
            err = "Replacement failed and temp output is gone (error code " + std::to_string(replace_err) +
                  "); check backup " + utf8_from_path(bak);
            return false;
        }

        if (MoveFileExW(target.c_str(), bak.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            if (MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                std::filesystem::remove(bak, ec);
                return true;
            }
            DWORD move_err = GetLastError();
            // Rollback: prove original bytes are back at target, keep backup otherwise.
            bool restored = MoveFileExW(bak.c_str(), target.c_str(),
                                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
            std::filesystem::remove(tmp, ec);
            if (restored && std::filesystem::exists(target, ec) && !std::filesystem::exists(bak, bak_ec)) {
                err = "Replacement failed, original restored (error code " + std::to_string(move_err) + ")";
            } else {
                err = "Replacement FAILED AND ROLLBACK UNVERIFIED (error code " + std::to_string(move_err) +
                      "). Original is at recovery backup " + utf8_from_path(bak) + " — DO NOT DELETE";
            }
            return false;
        }
        DWORD stage_err = GetLastError();
        std::filesystem::remove(tmp, ec);
        err = "Cannot stage target for replacement (error code " + std::to_string(stage_err) + ")";
        return false;
    } else {
        if (MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        std::filesystem::remove(tmp, ec);
        err = "Move to target failed (error code " + std::to_string(GetLastError()) + ")";
        return false;
    }
#else
    if (std::filesystem::exists(target, ec)) {
        std::filesystem::path bak = target;
        bak += L".bak_" + std::to_wstring(seq) + L".bzconv";
        std::error_code bak_ec;
        if (std::filesystem::exists(bak, bak_ec)) {
            std::filesystem::remove(tmp, ec);
            err = "Recovery backup path already occupied: " + utf8_from_path(bak);
            return false;
        }
        std::filesystem::rename(target, bak, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            err = "Cannot stage target for replacement: " + ec.message();
            return false;
        }
        std::filesystem::rename(tmp, target, ec);
        if (ec) {
            std::error_code rec;
            std::filesystem::rename(bak, target, rec);
            std::filesystem::remove(tmp, ec);
            if (!rec && std::filesystem::exists(target, rec) && !std::filesystem::exists(bak, rec)) {
                err = "Replacement failed, original restored: " + ec.message();
            } else {
                err = "Replacement FAILED AND ROLLBACK UNVERIFIED. Original at " + utf8_from_path(bak) + " — DO NOT DELETE";
            }
            return false;
        }
        std::filesystem::remove(bak, ec);
        return true;
    } else {
        std::filesystem::rename(tmp, target, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            err = "Rename to target failed: " + ec.message();
            return false;
        }
        return true;
    }
#endif
}

inline bool is_path_within(const std::filesystem::path& child, const std::filesystem::path& root) {
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(child, ec);
    auto r = std::filesystem::weakly_canonical(root, ec);
    auto cs = to_lower(utf8_from_path(c));
    auto rs = to_lower(utf8_from_path(r));
    if (!rs.empty() && rs.back() != '\\' && rs.back() != '/') rs.push_back('\\');
    std::replace(cs.begin(), cs.end(), '/', '\\');
    std::replace(rs.begin(), rs.end(), '/', '\\');
    return cs == rs.substr(0, rs.size() - 1) || cs.rfind(rs, 0) == 0;
}

} // namespace bz
