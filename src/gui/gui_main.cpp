// bzconv_gui - Golden Default (Wintage) Win32 front-end for bzconverter.
// Verdana, non-antialiased, 2px bevels, token-only palette.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "bzconverter/core/types.hpp"
#include "bzconverter/exporters/exporter.hpp"
#include "bzconverter/materials/material_resolver.hpp"
#include "bzconverter/validation/validator.hpp"

#include <windows.h>
#include <uxtheme.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <map>
#include <unordered_set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Golden Default palette, canonical Wintage theme tokens.
constexpr COLORREF T_BACKGROUND      = RGB(0x1A, 0x18, 0x10);
constexpr COLORREF T_SURFACE         = RGB(0x33, 0x2E, 0x22);
constexpr COLORREF T_SURFACE_RAISED  = RGB(0x3D, 0x37, 0x2A);
// Backwards-compatible alias used by the palette-driven paint code.
constexpr COLORREF T_SURFACE_RAIRED = T_SURFACE_RAISED;
constexpr COLORREF T_BORDER_DARK     = RGB(0x10, 0x0E, 0x08);
constexpr COLORREF T_BORDER_HI       = RGB(0xF0, 0xD0, 0x60);
constexpr COLORREF T_BEVEL_LIGHT     = RGB(0x75, 0x66, 0x3D);
constexpr COLORREF T_TEXT            = RGB(0xD4, 0xC8, 0x9A);
constexpr COLORREF T_TEXT_MUTED      = RGB(0x6E, 0x67, 0x4E);
constexpr COLORREF T_ACCENT_TEAL     = RGB(0x00, 0x80, 0x80);
constexpr COLORREF T_ACCENT_TEAL_DP  = RGB(0x00, 0x4C, 0x4C);
constexpr COLORREF T_DANGER          = RGB(0x7A, 0x20, 0x20);
constexpr COLORREF T_DANGER_TEXT     = RGB(0xD6, 0x64, 0x64);
constexpr COLORREF T_COMPARE_BACK    = RGB(0x14, 0x12, 0x0C);

enum : int {
    ID_QUEUE = 200, ID_ADD_FILES, ID_ADD_FOLDER, ID_REMOVE, ID_CLEAR,
    ID_LBL_OUT, ID_OUT_EDIT, ID_OUT_BROWSE,
    ID_LBL_FORMAT, ID_FMT0, ID_FMT1, ID_FMT2, ID_FMT3, ID_ANIM, ID_MAT, ID_TEX, ID_OVERWRITE, ID_AXIS,
    ID_LBL_BLENDER, ID_BLENDER_EDIT, ID_BLENDER_BROWSE,
    ID_LBL_COMPANION, ID_COMPANION_EDIT, ID_COMPANION_BROWSE,
    ID_LBL_GAME, ID_GAME_EDIT, ID_LBL_SEARCH, ID_SEARCH_EDIT,
    ID_PROGRESS, ID_STATUS, ID_CONVERT, ID_CANCEL, ID_OPEN_OUT, ID_LOG
};

constexpr UINT WM_APP_LOG      = WM_APP + 1;
constexpr UINT WM_APP_ITEM     = WM_APP + 2;
constexpr UINT WM_APP_PROGRESS = WM_APP + 3;
constexpr UINT WM_APP_DONE     = WM_APP + 4;

const wchar_t* FORMATS[] = {L"FBX", L"GLB", L"glTF", L"OBJ"};
const wchar_t* EXTS[]    = {L".fbx", L".glb", L".gltf", L".obj"};
const bz::ExportFormat FMT_VALUES[] = {bz::ExportFormat::FBX, bz::ExportFormat::GLB,
                                       bz::ExportFormat::GLTF, bz::ExportFormat::OBJ};

struct Job { std::wstring input, output; };

struct Settings {
    std::wstring output_dir, blender, companion, game_roots, search_roots;
    int format = 0;
    bool anim = true, materials = true, textures = true, overwrite = false, axis = false;
};

struct WorkerOptions {
    int format = 0;
    bool anim = true, materials = true, textures = true, overwrite = false, axis = false;
    std::wstring blender, companion, game_roots, search_roots;
};

HFONT g_font = nullptr;
HBRUSH g_br_bg = nullptr, g_br_field = nullptr, g_br_surface = nullptr, g_br_raised = nullptr;
HWND g_hwnd = nullptr, g_queue = nullptr, g_out = nullptr,
     g_anim = nullptr, g_mat = nullptr, g_tex = nullptr, g_overwrite = nullptr, g_axis = nullptr,
     g_blender = nullptr, g_companion = nullptr, g_game = nullptr, g_search = nullptr,
     g_progress = nullptr, g_status = nullptr, g_convert = nullptr, g_cancel = nullptr,
     g_log = nullptr, g_labels[16] = {};
int g_label_count = 0;
int g_progress_value = 0;
std::map<int, bool> g_check;

std::vector<Job> g_jobs;
std::thread g_worker;
std::atomic<bool> g_cancel_flag{false}, g_worker_active{false};
bool g_closing = false;
Settings g_settings;
int g_dpi = 96;

fs::path ini_path() {
    wchar_t buf[MAX_PATH * 4] = L"";
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(buf).parent_path() / L"bzconv.ini";
}

std::wstring ini_get(const wchar_t* key, const wchar_t* def) {
    std::vector<wchar_t> buf(4096, L'\0');
    GetPrivateProfileStringW(L"bzconv", key, def, buf.data(), static_cast<DWORD>(buf.size()), ini_path().c_str());
    return std::wstring(buf.data());
}

void ini_set(const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(L"bzconv", key, value.c_str(), ini_path().c_str());
}

void load_settings() {
    g_settings.output_dir = ini_get(L"outputDir", L"");
    g_settings.blender = ini_get(L"blender", L"");
    g_settings.companion = ini_get(L"companion", L"");
    g_settings.game_roots = ini_get(L"gameRoots", L"");
    g_settings.search_roots = ini_get(L"searchRoots", L"");
    g_settings.format = std::clamp<int>(GetPrivateProfileIntW(L"bzconv", L"format", 0, ini_path().c_str()), 0, 3);
    g_settings.anim = GetPrivateProfileIntW(L"bzconv", L"anim", 1, ini_path().c_str()) != 0;
    g_settings.materials = GetPrivateProfileIntW(L"bzconv", L"materials", 1, ini_path().c_str()) != 0;
    g_settings.textures = GetPrivateProfileIntW(L"bzconv", L"textures", 1, ini_path().c_str()) != 0;
    g_settings.overwrite = GetPrivateProfileIntW(L"bzconv", L"overwrite", 0, ini_path().c_str()) != 0;
    g_settings.axis = GetPrivateProfileIntW(L"bzconv", L"axis", 0, ini_path().c_str()) != 0;
}

void save_settings() {
    ini_set(L"outputDir", g_settings.output_dir);
    ini_set(L"blender", g_settings.blender);
    ini_set(L"companion", g_settings.companion);
    ini_set(L"gameRoots", g_settings.game_roots);
    ini_set(L"searchRoots", g_settings.search_roots);
    ini_set(L"format", std::to_wstring(g_settings.format));
    ini_set(L"anim", g_settings.anim ? L"1" : L"0");
    ini_set(L"materials", g_settings.materials ? L"1" : L"0");
    ini_set(L"textures", g_settings.textures ? L"1" : L"0");
    ini_set(L"overwrite", g_settings.overwrite ? L"1" : L"0");
    ini_set(L"axis", g_settings.axis ? L"1" : L"0");
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::vector<wchar_t> w(static_cast<size_t>(n) + 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return std::wstring(w.data(), static_cast<size_t>(n));
}

std::wstring get_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return {};
    std::vector<wchar_t> buf(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, buf.data(), static_cast<int>(buf.size()));
    return std::wstring(buf.data(), static_cast<size_t>(n));
}

void set_text(HWND h, const std::wstring& text) { SetWindowTextW(h, text.c_str()); }

bool post_text(UINT msg, const std::wstring& text) {
    auto* p = new (std::nothrow) std::wstring(text);
    if (!p) return false;
    if (!PostMessageW(g_hwnd, msg, 0, reinterpret_cast<LPARAM>(p))) { delete p; return false; }
    return true;
}

void post_log(const std::wstring& line) { post_text(WM_APP_LOG, line); }
void post_item(const std::wstring& line) { post_text(WM_APP_ITEM, line); }

void append_log(const std::wstring& line) {
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    std::wstring t = line + L"\r\n";
    SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(t.c_str()));
}

int S(int px) { return MulDiv(px, g_dpi, 96); }

bool has_model_ext(const std::wstring& p) {
    std::wstring lower = p;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::tolower(c)); });
    return lower.size() > 4 && (lower.compare(lower.size() - 4, 4, L".xsi") == 0 ||
                                lower.compare(lower.size() - 4, 4, L".msh") == 0);
}

std::wstring normalized_path(const fs::path& p) {
    std::error_code ec;
    auto c = fs::weakly_canonical(p, ec);
    if (ec) c = p;
    std::wstring s = c.wstring();
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::tolower(ch)); });
    std::replace(s.begin(), s.end(), L'/', L'\\');
    return s;
}

std::vector<fs::path> split_paths(const std::wstring& text) {
    std::vector<fs::path> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L';', start);
        std::wstring part = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!part.empty()) out.push_back(fs::path(part));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return out;
}

void refresh_queue();
std::wstring compute_output_path(const std::wstring& input, int format, const std::wstring& out_dir) {
    fs::path out(input);
    out.replace_extension(EXTS[format]);
    if (!out_dir.empty()) out = fs::path(out_dir) / out.filename();
    return out.wstring();
}
void recompute_all_outputs() {
    for (auto& j : g_jobs) j.output = compute_output_path(j.input, g_settings.format, g_settings.output_dir);
    refresh_queue();
}
std::wstring job_to_string(const Job& j) { return j.input + L"  ->  " + j.output; }

void refresh_queue() {
    SendMessageW(g_queue, LB_RESETCONTENT, 0, 0);
    for (const auto& j : g_jobs)
        SendMessageW(g_queue, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(job_to_string(j).c_str()));
}

void add_job(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || !has_model_ext(p.wstring())) return;
    std::wstring key = normalized_path(p);
    for (const auto& j : g_jobs) if (normalized_path(j.input) == key) return;
    g_jobs.push_back({p.wstring(), compute_output_path(p.wstring(), g_settings.format, g_settings.output_dir)});
}

void add_directory(const fs::path& dir) {
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) if (e.is_regular_file(ec)) add_job(e.path());
}

void browse_files() {
    std::vector<wchar_t> buf(128 * 1024, L'\0');
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Battlezone models (*.xsi;*.msh)\0*.xsi;*.msh\0All files\0*.*\0\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    wchar_t* p = buf.data();
    std::wstring first = p;
    p += first.size() + 1;
    if (*p == L'\0') add_job(first);
    else {
        std::wstring dir = first;
        while (*p) {
            std::wstring name = p;
            add_job(fs::path(dir) / name);
            p += name.size() + 1;
        }
    }
    refresh_queue();
}

void browse_folder(bool output_mode) {
    wchar_t path[MAX_PATH * 4] = L"";
    BROWSEINFOW bi{};
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = output_mode ? L"Choose output directory" : L"Choose source directory";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (output_mode) {
        set_text(g_out, path);
        for (auto& j : g_jobs) j.output = compute_output_path(j.input, g_settings.format, path);
    } else {
        add_directory(path);
    }
    refresh_queue();
}

void browse_file_into(HWND target, const wchar_t* filter, const wchar_t* title) {
    wchar_t buf[MAX_PATH * 4] = L"";
    std::wstring cur = get_text(target);
    if (!cur.empty()) wcsncpy_s(buf, cur.c_str(), std::size(buf) - 1);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrTitle = title;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buf));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) set_text(target, buf);
}

void run_batch(std::vector<Job> jobs, WorkerOptions o) {
    bz::ResolverConfig rcfg;
    rcfg.game_asset_roots = bz::MaterialResolver::detect_default_game_roots();
    rcfg.workshop_root = bz::MaterialResolver::detect_default_workshop_root();
    rcfg.search_workshop = !rcfg.workshop_root.empty();
    for (const auto& p : split_paths(o.game_roots)) rcfg.game_asset_roots.push_back(p);
    for (const auto& p : split_paths(o.search_roots)) rcfg.user_search_roots.push_back(p);
    bz::MaterialResolver resolver(rcfg);

    bz::ExportOptions ex;
    ex.format = FMT_VALUES[o.format];
    ex.include_animations = o.anim;
    ex.include_materials = o.materials;
    ex.include_textures = o.textures;
    ex.overwrite = o.overwrite;
    ex.bzcc_axis_mode = o.axis;
    if (!o.blender.empty()) ex.blender_exe = fs::path(o.blender);
    if (!o.companion.empty()) ex.companion_model = fs::path(o.companion);

    size_t done = 0, ok = 0, partial = 0, failed = 0, cancelled = 0;
    for (const auto& job : jobs) {
        if (g_cancel_flag.load()) { ++cancelled; post_item(L"Cancelled by user"); break; }
        post_item(L"[" + std::to_wstring(done + 1) + L"/" + std::to_wstring(jobs.size()) + L"] " + fs::path(job.input).filename().wstring());
        bz::ExportResult r = bz::convert_single_file(fs::path(job.input), fs::path(job.output), ex, resolver, &g_cancel_flag);
        for (const auto& d : r.diagnostics) {
            const wchar_t* sev = d.severity == bz::DiagSeverity::Error ? L"ERROR" : d.severity == bz::DiagSeverity::Warning ? L"WARN" : L"INFO";
            post_log(std::wstring(L"  [") + sev + L"] " + wide(d.code) + L": " + wide(d.message));
        }
        post_log(L"  " + wide(bz::result_class_to_string(r.result_class)) + L": " + wide(r.summary));
        if (r.status == bz::JobStatus::Cancelled) ++cancelled;
        else if (r.status == bz::JobStatus::Partial) ++partial;
        else if (r.status == bz::JobStatus::Failed) ++failed;
        else ++ok;
        ++done;
        PostMessageW(g_hwnd, WM_APP_PROGRESS, static_cast<WPARAM>(done * 100 / jobs.size()), 0);
    }
    post_item(L"Finished: " + std::to_wstring(ok) + L" success, " + std::to_wstring(partial) +
              L" partial, " + std::to_wstring(failed) + L" failed, " + std::to_wstring(cancelled) + L" cancelled.");
    PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

void drain_app_messages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, WM_APP_LOG, WM_APP_DONE, PM_REMOVE)) {
        if (msg.message == WM_APP_LOG || msg.message == WM_APP_ITEM) delete reinterpret_cast<std::wstring*>(msg.lParam);
    }
}

void finish_worker(bool closing) {
    if (g_worker.joinable()) g_worker.join();
    g_worker_active = false;
    g_cancel_flag = false;
    EnableWindow(g_convert, TRUE);
    EnableWindow(g_cancel, FALSE);
    EnableWindow(g_queue, TRUE);
    if (closing) { drain_app_messages(); DestroyWindow(g_hwnd); }
}

void start_conversion() {
    if (g_worker_active.load()) return;
    if (g_jobs.empty()) { append_log(L"Error: queue is empty. Add XSI/MSH files first."); return; }
    g_settings.anim = g_check[ID_ANIM]; g_settings.materials = g_check[ID_MAT];
    g_settings.textures = g_check[ID_TEX]; g_settings.overwrite = g_check[ID_OVERWRITE];
    g_settings.axis = g_check[ID_AXIS];
    g_settings.blender = get_text(g_blender);
    g_settings.companion = get_text(g_companion);
    g_settings.output_dir = get_text(g_out);
    g_settings.game_roots = get_text(g_game);
    g_settings.search_roots = get_text(g_search);

    // Recompute every destination from the CURRENT configuration before launch.
    recompute_all_outputs();
    std::unordered_set<std::wstring> seen_outputs;
    for (const auto& j : g_jobs) {
        std::error_code ec;
        if (!fs::is_regular_file(j.input, ec)) {
            append_log(L"Preflight failed: input missing: " + j.input); return;
        }
        fs::path op(j.output);
        if (bz::to_lower(bz::utf8_from_path(op.extension())) != "." + std::string(bz::export_format_to_string(FMT_VALUES[g_settings.format]))) {
            append_log(L"Preflight failed: output extension does not match selected format: " + j.output); return;
        }
        if (normalized_path(j.input) == normalized_path(j.output)) {
            append_log(L"Preflight failed: input and output are the same file: " + j.input); return;
        }
        if (op.has_parent_path()) {
            std::filesystem::create_directories(op.parent_path(), ec);
            if (ec || !fs::is_directory(op.parent_path(), ec)) {
                append_log(L"Preflight failed: cannot create output directory: " + op.parent_path().wstring()); return;
            }
        }
        std::wstring key = normalized_path(j.output);
        if (!seen_outputs.insert(key).second) {
            append_log(L"Preflight failed: output collision: " + j.output); return;
        }
    }
    if (!g_settings.overwrite) {
        auto fail_existing = [&](const fs::path& p, const wchar_t* role) -> bool {
            std::error_code xec;
            if (!fs::exists(p, xec)) return false;
            append_log(std::wstring(L"Preflight: ") + role + L" exists and overwrite is disabled: " + p.wstring());
            return true;
        };
        for (const auto& j : g_jobs) {
            if (fail_existing(j.output, L"output")) return;
            fs::path op(j.output);
            auto fmt = FMT_VALUES[g_settings.format];
            if (fmt == bz::ExportFormat::GLTF) {
                fs::path bin = op; bin.replace_extension(L".bin");
                if (fail_existing(bin, L"glTF binary sidecar")) return;
            } else if (fmt == bz::ExportFormat::OBJ && g_settings.materials) {
                fs::path mtl = op; mtl.replace_extension(L".mtl");
                if (fail_existing(mtl, L"OBJ material sidecar")) return;
            }
            // Texture targets depend on resolved material channels. The
            // exporter's OutputBundle preflights exactly those planned paths;
            // scanning the whole _textures directory would block unrelated
            // files that this conversion never touches.
        }
    }
    save_settings();

    WorkerOptions o;
    o.format = g_settings.format;
    o.anim = g_settings.anim; o.materials = g_settings.materials; o.textures = g_settings.textures;
    o.overwrite = g_settings.overwrite; o.axis = g_settings.axis;
    o.blender = g_settings.blender; o.companion = g_settings.companion;
    o.game_roots = g_settings.game_roots; o.search_roots = g_settings.search_roots;

    g_cancel_flag = false;
    g_worker_active = true;
    EnableWindow(g_convert, FALSE);
    EnableWindow(g_cancel, TRUE);
    EnableWindow(g_queue, FALSE);
    g_progress_value = 0; if (g_progress) InvalidateRect(g_progress, nullptr, FALSE);
    set_text(g_status, L"Converting...");
    g_worker = std::thread(run_batch, g_jobs, std::move(o));
}

void request_cancel() {
    if (!g_worker_active.load()) return;
    g_cancel_flag = true;
    set_text(g_status, L"Cancelling - waiting for active operation...");
    EnableWindow(g_cancel, FALSE);
}

void draw_bevel(HDC dc, RECT r, bool sunken) {
    COLORREF tl = sunken ? T_BORDER_DARK : T_BEVEL_LIGHT;
    COLORREF br = sunken ? T_BEVEL_LIGHT : T_BORDER_DARK;
    for (int i = 0; i < 2; ++i) {
        HPEN p1 = CreatePen(PS_SOLID, 1, tl), p2 = CreatePen(PS_SOLID, 1, br);
        HGDIOBJ old = SelectObject(dc, p1);
        MoveToEx(dc, r.left + i, r.bottom - 1 - i, nullptr);
        LineTo(dc, r.left + i, r.top + i);
        LineTo(dc, r.right - 1 - i, r.top + i);
        SelectObject(dc, p2);
        LineTo(dc, r.right - 1 - i, r.bottom - 1 - i);
        LineTo(dc, r.left + i, r.bottom - 1 - i);
        SelectObject(dc, old);
        DeleteObject(p1); DeleteObject(p2);
    }
}

void draw_check(const DRAWITEMSTRUCT* di) {
    RECT r = di->rcItem;
    HBRUSH bg = CreateSolidBrush(T_BACKGROUND); FillRect(di->hDC, &r, bg); DeleteObject(bg);
    RECT box{r.left + 2, r.top + (r.bottom - r.top - 14) / 2, r.left + 16, r.top + (r.bottom - r.top - 14) / 2 + 14};
    HBRUSH f = CreateSolidBrush(T_COMPARE_BACK); FillRect(di->hDC, &box, f); DeleteObject(f);
    bool checked = g_check[di->CtlID];
    HPEN p = CreatePen(PS_SOLID, 1, checked ? T_BORDER_HI : T_BEVEL_LIGHT);
    HGDIOBJ old = SelectObject(di->hDC, p);
    Rectangle(di->hDC, box.left, box.top, box.right, box.bottom);
    if (checked) {
        MoveToEx(di->hDC, box.left + 3, box.top + 7, nullptr);
        LineTo(di->hDC, box.left + 6, box.top + 11); LineTo(di->hDC, box.left + 12, box.top + 2);
    }
    SelectObject(di->hDC, old); DeleteObject(p);
    wchar_t label[64] = L""; GetWindowTextW(di->hwndItem, label, 64);
    RECT tr = r; tr.left += 22;
    SetBkMode(di->hDC, TRANSPARENT); SetTextColor(di->hDC, T_TEXT); SelectObject(di->hDC, g_font);
    DrawTextW(di->hDC, label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}
void draw_progress_bar(const DRAWITEMSTRUCT* di) {
    RECT r = di->rcItem;
    HBRUSH bg = CreateSolidBrush(T_COMPARE_BACK); FillRect(di->hDC, &r, bg); DeleteObject(bg);
    draw_bevel(di->hDC, r, true);
    RECT bar{r.left + 3, r.top + 3, r.left + 3 + (r.right - r.left - 6) * g_progress_value / 100, r.bottom - 3};
    if (bar.right > bar.left) { HBRUSH b = CreateSolidBrush(T_ACCENT_TEAL); FillRect(di->hDC, &bar, b); DeleteObject(b); }
}
void draw_queue_row(const DRAWITEMSTRUCT* di) {
    RECT r = di->rcItem;
    bool sel = (di->itemState & ODS_SELECTED) != 0;
    HBRUSH b = CreateSolidBrush(sel ? T_SURFACE_RAISED : T_SURFACE); FillRect(di->hDC, &r, b); DeleteObject(b);
    LRESULT len = SendMessageW(g_queue, LB_GETTEXTLEN, di->itemID, 0);
    if (len == LB_ERR || len < 0) return;
    std::vector<wchar_t> buf(static_cast<size_t>(len) + 1, L'\0');
    if (SendMessageW(g_queue, LB_GETTEXT, di->itemID, reinterpret_cast<LPARAM>(buf.data())) == LB_ERR) return;
    RECT tr = r; tr.left += 4;
    SetBkMode(di->hDC, TRANSPARENT); SetTextColor(di->hDC, T_TEXT); SelectObject(di->hDC, g_font);
    DrawTextW(di->hDC, buf.data(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}
void draw_button(const DRAWITEMSTRUCT* di) {
    bool pressed = (di->itemState & ODS_SELECTED) != 0;
    bool disabled = (di->itemState & ODS_DISABLED) != 0;
    int id = di->CtlID;
    if (id >= ID_FMT0 && id <= ID_FMT3 && id - ID_FMT0 == g_settings.format) pressed = true;
    COLORREF fill = disabled ? T_SURFACE : id == ID_CONVERT ? T_ACCENT_TEAL : id == ID_CANCEL ? T_DANGER : T_SURFACE_RAISED;
    if (pressed && !disabled) fill = id == ID_CONVERT ? T_ACCENT_TEAL_DP : T_SURFACE;
    HBRUSH b = CreateSolidBrush(fill);
    FillRect(di->hDC, &di->rcItem, b);
    DeleteObject(b);
    draw_bevel(di->hDC, di->rcItem, pressed);
    wchar_t text[128] = L"";
    GetWindowTextW(di->hwndItem, text, 128);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, disabled ? T_TEXT_MUTED : id == ID_CANCEL ? T_DANGER_TEXT : T_TEXT);
    SelectObject(di->hDC, g_font);
    RECT r = di->rcItem;
    if (pressed) OffsetRect(&r, 1, 1);
    DrawTextW(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (di->itemState & ODS_FOCUS) {
        RECT f = di->rcItem; InflateRect(&f, -4, -4);
        HBRUSH hb = CreateSolidBrush(T_BORDER_HI);
        FrameRect(di->hDC, &f, hb);
        DeleteObject(hb);
    }
}

HWND label(const wchar_t* text) {
    HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, 0, 0, 10, 10,
                             g_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1000 + g_label_count++)),
                             GetModuleHandleW(nullptr), nullptr);
    if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    if (g_label_count < static_cast<int>(std::size(g_labels))) g_labels[g_label_count - 1] = h;
    return h;
}

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                             g_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return c;
}

void place(HWND h, int x, int y, int w, int ht) { if (h) MoveWindow(h, S(x), S(y), S(w), S(ht), TRUE); }

void layout() {
    RECT cr; GetClientRect(g_hwnd, &cr);
    int W = static_cast<int>(cr.right / (g_dpi / 96.0));
    int H = static_cast<int>(cr.bottom / (g_dpi / 96.0));
    int m = 12, gap = 8, btn = 94, row = 24;
    int qh = std::max(110, static_cast<int>(H * 0.24));
    int y = 12;
    HWND add_files = GetDlgItem(g_hwnd, ID_ADD_FILES), add_folder = GetDlgItem(g_hwnd, ID_ADD_FOLDER),
         remove_btn = GetDlgItem(g_hwnd, ID_REMOVE), clear_btn = GetDlgItem(g_hwnd, ID_CLEAR);
    place(add_files, m, y, btn, row);
    place(add_folder, m + btn + gap, y, btn, row);
    place(remove_btn, m + 2 * (btn + gap), y, btn - 20, row);
    place(clear_btn, m + 3 * (btn + gap) - 20, y, btn - 20, row);
    y += row + 4;
    place(g_queue, m, y, W - 2 * m, qh);
    y += qh + 10;

    int fw = (W - 2 * m - gap) / 2;
    place(GetDlgItem(g_hwnd, ID_LBL_OUT), m, y + 4, 80, 16);
    place(g_out, m + 84, y, fw - 100, row);
    place(GetDlgItem(g_hwnd, ID_OUT_BROWSE), m + fw - 10, y, 90, row);
    place(GetDlgItem(g_hwnd, ID_LBL_FORMAT), m + fw + gap, y + 4, 44, 16);
    for (int i = 0; i < 4; ++i) place(GetDlgItem(g_hwnd, ID_FMT0 + i), m + fw + gap + 46 + i * 38, y, 36, row);
    y += row + 6;
    place(g_anim, m, y, 96, row);
    place(g_mat, m + 102, y, 90, row);
    place(g_tex, m + 198, y, 90, row);
    place(g_overwrite, m + fw + gap, y, 150, row);
    y += row + 4;
    place(g_axis, m, y, 100, row);
    y += row + 8;

    place(GetDlgItem(g_hwnd, ID_LBL_BLENDER), m, y + 4, 80, 16);
    place(g_blender, m + 84, y, fw - 100, row);
    place(GetDlgItem(g_hwnd, ID_BLENDER_BROWSE), m + fw - 10, y, 90, row);
    place(GetDlgItem(g_hwnd, ID_LBL_COMPANION), m + fw + gap, y + 4, 82, 16);
    place(g_companion, m + fw + gap + 86, y, fw - 96, row);
    place(GetDlgItem(g_hwnd, ID_COMPANION_BROWSE), W - m - 90, y, 90, row);
    y += row + 6;

    place(GetDlgItem(g_hwnd, ID_LBL_GAME), m, y + 4, 80, 16);
    place(g_game, m + 84, y, fw - 100, row);
    place(GetDlgItem(g_hwnd, ID_LBL_SEARCH), m + fw + gap, y + 4, 80, 16);
    place(g_search, m + fw + gap + 84, y, fw - 94, row);
    y += row + 10;

    place(g_progress, m, y, W - 2 * m, 20);
    y += 26;
    place(g_status, m, y, W - 2 * m, 18);
    y += 24;
    place(g_convert, m, y, 140, 30);
    place(g_cancel, m + 148, y, 100, 30);
    place(GetDlgItem(g_hwnd, ID_OPEN_OUT), m + 256, y, 150, 30);
    y += 38;
    place(g_log, m, y, W - 2 * m, std::max(90, H - y - m));
}

HWND get_control(int id) { return GetDlgItem(g_hwnd, id); }

void build_ui() {
    make(L"BUTTON", L"Add files", WS_TABSTOP | BS_OWNERDRAW, ID_ADD_FILES);
    make(L"BUTTON", L"Add folder", WS_TABSTOP | BS_OWNERDRAW, ID_ADD_FOLDER);
    make(L"BUTTON", L"Remove", WS_TABSTOP | BS_OWNERDRAW, ID_REMOVE);
    make(L"BUTTON", L"Clear", WS_TABSTOP | BS_OWNERDRAW, ID_CLEAR);
    g_queue = make(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_EXTENDEDSEL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, ID_QUEUE);
    label(L"Output dir:");
    g_out = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_OUT_EDIT);
    make(L"BUTTON", L"Browse...", WS_TABSTOP | BS_OWNERDRAW, ID_OUT_BROWSE);
    label(L"Format:");
    for (int i = 0; i < 4; ++i) make(L"BUTTON", FORMATS[i], WS_TABSTOP | BS_OWNERDRAW, ID_FMT0 + i);
    g_anim = make(L"BUTTON", L"Animations", WS_TABSTOP | BS_OWNERDRAW, ID_ANIM);
    g_mat = make(L"BUTTON", L"Materials", WS_TABSTOP | BS_OWNERDRAW, ID_MAT);
    g_tex = make(L"BUTTON", L"Textures", WS_TABSTOP | BS_OWNERDRAW, ID_TEX);
    g_overwrite = make(L"BUTTON", L"Overwrite existing", WS_TABSTOP | BS_OWNERDRAW, ID_OVERWRITE);
    g_axis = make(L"BUTTON", L"BZCC axis", WS_TABSTOP | BS_OWNERDRAW, ID_AXIS);
    g_check[ID_ANIM]=g_settings.anim; g_check[ID_MAT]=g_settings.materials;
    g_check[ID_TEX]=g_settings.textures; g_check[ID_OVERWRITE]=g_settings.overwrite;
    g_check[ID_AXIS]=g_settings.axis;
    label(L"Blender:");
    g_blender = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_BLENDER_EDIT);
    make(L"BUTTON", L"Blender...", WS_TABSTOP | BS_OWNERDRAW, ID_BLENDER_BROWSE);
    label(L"Companion:");
    g_companion = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_COMPANION_EDIT);
    make(L"BUTTON", L"Companion...", WS_TABSTOP | BS_OWNERDRAW, ID_COMPANION_BROWSE);
    label(L"Game paths:");
    g_game = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_GAME_EDIT);
    label(L"Search paths:");
    g_search = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_SEARCH_EDIT);
    g_progress = make(L"STATIC", L"", SS_OWNERDRAW, ID_PROGRESS);
    g_status = make(L"STATIC", L"Ready - add XSI/MSH files or drag them onto the window.", 0, ID_STATUS);
    g_convert = make(L"BUTTON", L"Convert queue", WS_TABSTOP | BS_OWNERDRAW, ID_CONVERT);
    g_cancel = make(L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW, ID_CANCEL);
    make(L"BUTTON", L"Open output folder", WS_TABSTOP | BS_OWNERDRAW, ID_OPEN_OUT);
    g_log = make(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, ID_LOG);
    EnableWindow(g_cancel, FALSE);

    set_text(g_out, g_settings.output_dir);
    set_text(g_blender, g_settings.blender);
    set_text(g_companion, g_settings.companion);
    set_text(g_game, g_settings.game_roots);
    set_text(g_search, g_settings.search_roots);
    SetWindowTheme(g_out, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(g_blender, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(g_companion, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(g_game, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(g_search, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(g_log, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(g_queue, L"DarkMode_Explorer", nullptr);
    append_log(L"Golden Default / Wintage. Add files, choose format, press Convert queue.");
    layout();
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_hwnd = h;
        DragAcceptFiles(h, TRUE);
        build_ui();
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(w);
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t buf[MAX_PATH * 4] = L"";
            DragQueryFileW(drop, i, buf, static_cast<UINT>(std::size(buf)));
            std::error_code ec;
            if (fs::is_directory(buf, ec)) add_directory(buf);
            else add_job(buf);
        }
        DragFinish(drop);
        refresh_queue();
        set_text(g_status, L"Files added to queue.");
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(w) == EN_CHANGE && LOWORD(w) == ID_OUT_EDIT) {
            g_settings.output_dir = get_text(g_out);
            recompute_all_outputs();
            return 0;
        }
        switch (LOWORD(w)) {
        case ID_ANIM: case ID_MAT: case ID_TEX: case ID_OVERWRITE: case ID_AXIS: {
            int id = LOWORD(w); g_check[id] = !g_check[id]; InvalidateRect(GetDlgItem(h, id), nullptr, FALSE);
            return 0;
        }
        case ID_FMT0: case ID_FMT1: case ID_FMT2: case ID_FMT3: {
            g_settings.format = LOWORD(w) - ID_FMT0;
            recompute_all_outputs();
            for (int i = 0; i < 4; ++i) InvalidateRect(GetDlgItem(h, ID_FMT0 + i), nullptr, FALSE);
            return 0;
        }
        case ID_ADD_FILES: browse_files(); return 0;
        case ID_ADD_FOLDER: browse_folder(false); return 0;
        case ID_OUT_BROWSE: browse_folder(true); return 0;
        case ID_BLENDER_BROWSE: browse_file_into(g_blender, L"Blender\0blender.exe\0All files\0*.*\0\0", L"Select blender.exe"); return 0;
        case ID_COMPANION_BROWSE: browse_file_into(g_companion, L"Models\0*.xsi;*.msh\0\0", L"Select companion base model"); return 0;
        case ID_REMOVE: {
            int count = static_cast<int>(SendMessageW(g_queue, LB_GETSELCOUNT, 0, 0));
            if (count == LB_ERR || count == 0) return 0;
            std::vector<int> selected(static_cast<size_t>(count));
            int got = static_cast<int>(SendMessageW(g_queue, LB_GETSELITEMS, count,
                                                    reinterpret_cast<LPARAM>(selected.data())));
            if (got == LB_ERR) return 0;
            std::sort(selected.begin(), selected.begin() + got, std::greater<int>());
            for (int i = 0; i < got; ++i)
                if (selected[i] >= 0 && selected[i] < static_cast<int>(g_jobs.size()))
                    g_jobs.erase(g_jobs.begin() + selected[i]);
            refresh_queue();
            return 0;
        }
        case ID_CLEAR: g_jobs.clear(); refresh_queue(); return 0;
        case ID_CONVERT: start_conversion(); return 0;
        case ID_CANCEL: request_cancel(); return 0;
        case ID_OPEN_OUT: {
            std::wstring dir = get_text(g_out);
            if (dir.empty() && !g_jobs.empty()) dir = fs::path(g_jobs[0].input).parent_path().wstring();
            if (!dir.empty()) ShellExecuteW(h, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        }
        break;
    case WM_MEASUREITEM: {
        auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(l);
        if (mi->CtlType == ODT_LISTBOX) mi->itemHeight = 18;
        return TRUE;
    }
    case WM_DRAWITEM: {
        auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(l);
        if (di->CtlType == ODT_LISTBOX) { draw_queue_row(di); return TRUE; }
        if (di->CtlID == ID_PROGRESS) { draw_progress_bar(di); return TRUE; }
        if (di->CtlID == ID_ANIM || di->CtlID == ID_MAT || di->CtlID == ID_TEX ||
            di->CtlID == ID_OVERWRITE || di->CtlID == ID_AXIS) { draw_check(di); return TRUE; }
        draw_button(di); return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, T_TEXT);
        bool is_log = reinterpret_cast<HWND>(l) == g_log;
        bool is_status = reinterpret_cast<HWND>(l) == g_status;
        SetBkColor(dc, is_log ? T_COMPARE_BACK : is_status ? T_SURFACE_RAISED : T_BACKGROUND);
        return reinterpret_cast<LRESULT>(is_log ? g_br_field : is_status ? g_br_raised : g_br_bg);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, T_TEXT);
        bool is_queue = reinterpret_cast<HWND>(l) == g_queue;
        SetBkColor(dc, is_queue ? T_SURFACE : T_COMPARE_BACK);
        return reinterpret_cast<LRESULT>(is_queue ? g_br_surface : g_br_field);
    }
    case WM_CTLCOLORBTN: return reinterpret_cast<LRESULT>(g_br_bg);
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r);
        FillRect(reinterpret_cast<HDC>(w), &r, g_br_bg);
        return 1;
    }
    case WM_APP_LOG: {
        auto* s = reinterpret_cast<std::wstring*>(l);
        if (s) { append_log(*s); delete s; }
        return 0;
    }
    case WM_APP_ITEM: {
        auto* s = reinterpret_cast<std::wstring*>(l);
        if (s) { set_text(g_status, *s); append_log(*s); delete s; }
        return 0;
    }
    case WM_APP_PROGRESS:
        g_progress_value = static_cast<int>(w); if (g_progress) InvalidateRect(g_progress, nullptr, FALSE);
        return 0;
    case WM_APP_DONE:
        finish_worker(g_closing);
        return 0;
    case WM_CLOSE:
        save_settings();
        if (g_worker_active.load()) { g_closing = true; request_cancel(); return 0; }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        save_settings();
        g_cancel_flag = true;
        if (g_worker.joinable()) g_worker.join();
        drain_app_messages();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    SetProcessDPIAware();
    HDC screen = GetDC(nullptr);
    g_dpi = std::max(96, GetDeviceCaps(screen, LOGPIXELSX));
    ReleaseDC(nullptr, screen);

    g_font = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                         DEFAULT_PITCH | FF_SWISS, L"Verdana");
    g_br_bg = CreateSolidBrush(T_BACKGROUND);
    g_br_field = CreateSolidBrush(T_COMPARE_BACK);
    g_br_surface = CreateSolidBrush(T_SURFACE);
    g_br_raised = CreateSolidBrush(T_SURFACE_RAIRED);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_br_bg;
    wc.lpszClassName = L"BzConvGoldenDefault";
    RegisterClassW(&wc);

    load_settings();

    RECT r{0, 0, S(760), S(560)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
#ifndef BZCONV_VERSION
#define BZCONV_VERSION "0.0.1"
#endif
    std::wstring version_wide;
    for (const char* c = BZCONV_VERSION; *c; ++c) version_wide.push_back(static_cast<wchar_t>(*c));
    std::wstring title = L"bzconv " + version_wide + L" - Battlezone XSI/MSH Converter";
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(),
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    DeleteObject(g_font); DeleteObject(g_br_bg); DeleteObject(g_br_field);
    DeleteObject(g_br_surface); DeleteObject(g_br_raised);
    return static_cast<int>(msg.wParam);
}
