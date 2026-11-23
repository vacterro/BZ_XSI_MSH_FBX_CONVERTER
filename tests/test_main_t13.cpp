#pragma once
// T-13 RED/GREEN witnesses for the two P0 data-safety defects:
//   1. durable journal has no per-file commit checkpoint
//   2. prepare() destroys a pending recovery backup
// Plus recovery-evidence preservation and third-party-modification guards.
#include "bzconverter/core/output_bundle.hpp"
#include "bzconverter/core/sha256.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace bztests_t13 {

namespace fs = std::filesystem;

static int g_seq = 0;

static fs::path fresh_dir(const char* tag) {
    fs::path dir = fs::temp_directory_path() /
        (std::string("bzconv_t13_") + tag + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
         std::to_string(++g_seq));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

static bool put(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << s;
    return true;
}

static bool get(const fs::path& p, std::string& s) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    s.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static fs::path only_stage(const fs::path& dir) {
    std::error_code ec;
    for (const auto& it : fs::directory_iterator(dir, ec)) {
        if (!it.is_directory()) continue;
        std::string n = it.path().filename().string();
        if (n.size() > 13 && n.substr(n.size() - 13) == ".bzconv-stage") return it.path();
    }
    return {};
}

static std::error_code& ec0() { static std::error_code e; e.clear(); return e; }

static bz::Json legacy_journal(const fs::path& stage, const fs::path& final_path,
                               bool existed, bool committed,
                               const std::string& original_backup,
                               const std::string& staged_hash,
                               const std::string& original_hash) {
    bz::Json je = bz::Json::object();
    je["role"] = "witness";
    je["finalPath"] = bz::utf8_from_path(final_path);
    je["existed"] = existed;
    je["stagedTemp"] = bz::utf8_from_path(stage / "0_staged");
    je["originalBackup"] = original_backup.empty()
        ? bz::Json() : bz::Json(original_backup);
    je["committed"] = committed;
    if (!staged_hash.empty()) je["stagedSha256"] = staged_hash;
    if (!original_hash.empty()) je["originalSha256"] = original_hash;
    bz::Json jr = bz::Json::object();
    jr["format"] = "bzconv-bundle-journal";
    jr["version"] = staged_hash.empty() ? 1 : 2;
    jr["committedAll"] = false;
    jr["entries"] = bz::Json::array();
    jr["entries"].push_back(je);
    return jr;
}

static void assert_eq(const std::string& got, const std::string& want, const char* what, bool& ok) {
    if (got != want) {
        std::cerr << "  FAILED: " << what << " got '" << got << "' want '" << want << "'\n";
        ok = false;
    }
}

// RED witness 1: crash after first file. Journal still says every entry
// uncommitted, yet recovery must roll the whole bundle back.
bool test_durable_per_entry_commit_checkpoint() {
    bool ok = true;
    fs::path dir = fresh_dir("checkpoint");
    fs::path a = dir / "model.gltf";
    fs::path b = dir / "model.bin";
    put(a, "ORIGINAL_GLTF");
    put(b, "ORIGINAL_BIN");

    bz::OutputBundle bundle;
    bundle.add(a, std::vector<uint8_t>{'N', 'E', 'W', '_', 'G', 'L', 'T', 'F'}, "gltf");
    bundle.add(b, std::vector<uint8_t>{'N', 'E', 'W', '_', 'B', 'I', 'N'}, "bin");
    std::string err;
    if (!bundle.prepare(true, err)) {
        std::cerr << "  FAILED: prepare: " << err << "\n";
        return false;
    }
    fs::path stage = only_stage(dir);
    if (stage.empty()) { std::cerr << "  FAILED: no staging dir\n"; return false; }

    // Process dies after the first target was published. The second target is
    // still the original. No commit() call happened, so in-memory flags are
    // useless and the on-disk journal is the only witness.
    // Durable write-intent checkpoint before touching the first target.
    std::vector<uint8_t> jbytes;
    std::string jerr;
    bz::read_file_bytes(stage / "bundle-journal.json", jbytes, jerr);
    bz::Json jr;
    bz::Json::parse(std::string(jbytes.begin(), jbytes.end()), jr, jerr);
    jr["state"] = "committing";
    bz::Json::Array arr = jr["entries"].as_arr();
    arr[0]["phase"] = "write-intent";
    arr[0]["committed"] = false;
    jr["entries"] = std::move(arr);
    std::string jt = jr.dump(2);
    put(stage / "bundle-journal.json", jt);
    put(a, "NEW_GLTF");

    std::string report;
    if (!bz::OutputBundle::recover_pending(dir, report)) {
        std::cerr << "  FAILED: recover_pending: " << report << "\n";
        return false;
    }
    std::string got;
    get(a, got); assert_eq(got, "ORIGINAL_GLTF", "first target after recovery", ok);
    get(b, got); assert_eq(got, "ORIGINAL_BIN", "second target after recovery", ok);
    if (fs::exists(stage, ec0())) {
        std::cerr << "  FAILED: staging dir survived successful recovery\n";
        ok = false;
    }
    fs::remove_all(dir, ec0());
    return ok;
}

// RED witness 2: a second conversion with the same primary output path must
// never delete or overwrite an unresolved backup from an earlier crash.
bool test_prepare_preserves_pending_recovery() {
    bool ok = true;
    fs::path dir = fresh_dir("clobber");
    fs::path target = dir / "x.gltf";
    fs::path stage = dir / ".x.gltf.bzconv-stage";
    fs::create_directories(stage, ec0());
    put(target, "CRASHED_NEW");
    put(stage / "orig.bin", "ORIGINAL");
    { std::ofstream f(stage / "bundle-journal.json", std::ios::binary);
      f << legacy_journal(stage, target, true, true,
                          bz::utf8_from_path(stage / "orig.bin"), "", "").dump(2); }

    bz::OutputBundle bundle;
    bundle.add(target, std::vector<uint8_t>{'S', 'E', 'C', 'O', 'N', 'D'}, "gltf");
    std::string err;
    bool prepared = bundle.prepare(true, err);
    if (prepared) {
        std::cerr << "  FAILED: prepare accepted output with pending recovery\n";
        ok = false;
    }
    if (err.find("RECOVERY") == std::string::npos) {
        std::cerr << "  FAILED: err lacks RECOVERY reason: " << err << "\n";
        ok = false;
    }
    std::string backup;
    get(stage / "orig.bin", backup);
    assert_eq(backup, "ORIGINAL", "pre-crash original backup", ok);
    if (!fs::exists(stage / "bundle-journal.json", ec0())) {
        std::cerr << "  FAILED: pending journal was removed\n";
        ok = false;
    }
    fs::remove_all(dir, ec0());
    return ok;
}

// RED witness 3: failed rollback must keep journal, backups and an error
// report instead of erasing the only remaining recovery evidence.
bool test_failed_recovery_preserves_evidence() {
    bool ok = true;
    fs::path dir = fresh_dir("evidence");
    fs::path target = dir / "y.gltf";
    fs::path stage = dir / ".y.gltf.bzconv-stage";
    fs::create_directories(stage, ec0());
    put(target, "NEW");
    // Backup deliberately absent: rollback cannot prove original bytes.
    { std::ofstream f(stage / "bundle-journal.json", std::ios::binary);
      f << legacy_journal(stage, target, true, true,
                          bz::utf8_from_path(stage / "missing.bin"), "", "").dump(2); }

    std::string acted;
    if (bz::OutputBundle::recover_one(stage, acted)) {
        std::cerr << "  FAILED: recovery falsely reported success: " << acted << "\n";
        ok = false;
    }
    if (!fs::exists(stage, ec0()) ||
        !fs::exists(stage / "bundle-journal.json", ec0())) {
        std::cerr << "  FAILED: failed recovery destroyed its evidence\n";
        ok = false;
    }
    if (!fs::exists(stage / "recovery-error.txt", ec0())) {
        std::cerr << "  FAILED: failed recovery wrote no error report\n";
        ok = false;
    }
    fs::remove_all(dir, ec0());
    return ok;
}

// RED witness 4: target changed by an unknown writer after the crash. Current
// code restores the backup from the committed flag; correct code must fail
// closed and keep evidence.
bool test_third_party_modification_detected() {
    bool ok = true;
    fs::path dir = fresh_dir("thirdparty");
    fs::path target = dir / "z.gltf";
    fs::path stage = dir / ".z.gltf.bzconv-stage";
    fs::create_directories(stage, ec0());
    put(target, "SOMEONE_ELSES_EDIT");
    put(stage / "orig.bin", "ORIGINAL");
    { std::ofstream f(stage / "bundle-journal.json", std::ios::binary);
      f << legacy_journal(stage, target, true, true,
                          bz::utf8_from_path(stage / "orig.bin"),
                          bz::sha256_hex(reinterpret_cast<const uint8_t*>("NEW"), 3),
                          bz::sha256_hex(reinterpret_cast<const uint8_t*>("ORIGINAL"), 8)).dump(2); }

    std::string acted;
    if (bz::OutputBundle::recover_one(stage, acted)) {
        std::cerr << "  FAILED: unknown target content accepted: " << acted << "\n";
        ok = false;
    }
    std::string got;
    get(target, got);
    assert_eq(got, "SOMEONE_ELSES_EDIT", "third-party bytes must be untouched", ok);
    if (!fs::exists(stage, ec0())) {
        std::cerr << "  FAILED: evidence removed after failed provenance check\n";
        ok = false;
    }
    fs::remove_all(dir, ec0());
    return ok;
}

// RED witness 5: crash after all files written, before completion marker.
// Recovery must remove newly created files and restore existing ones.
bool test_crash_after_all_files_before_completion() {
    bool ok = true;
    fs::path dir = fresh_dir("allfiles");
    fs::path a = dir / "scene.obj";
    fs::path b = dir / "scene.mtl";
    put(a, "OLD_OBJ");
    bz::OutputBundle bundle;
    bundle.add(a, std::vector<uint8_t>{'N', 'E', 'W', '_', 'O', 'B', 'J'}, "obj");
    bundle.add(b, std::vector<uint8_t>{'N', 'E', 'W', '_', 'M', 'T', 'L'}, "mtl");
    std::string err;
    if (!bundle.prepare(true, err)) { std::cerr << "  FAILED: prepare: " << err << "\n"; return false; }
    fs::path stage = only_stage(dir);
    if (stage.empty()) { std::cerr << "  FAILED: no staging dir\n"; return false; }
    std::vector<uint8_t> jbytes;
    std::string jerr;
    bz::read_file_bytes(stage / "bundle-journal.json", jbytes, jerr);
    bz::Json jr;
    bz::Json::parse(std::string(jbytes.begin(), jbytes.end()), jr, jerr);
    jr["state"] = "committing";
    bz::Json::Array arr = jr["entries"].as_arr();
    for (auto& je : arr) { je["phase"] = "written"; je["committed"] = true; }
    jr["entries"] = std::move(arr);
    std::string jt = jr.dump(2);
    put(stage / "bundle-journal.json", jt);
    put(a, "NEW_OBJ");
    put(b, "NEW_MTL");
    std::string report;
    if (!bz::OutputBundle::recover_pending(dir, report)) {
        std::cerr << "  FAILED: recovery: " << report << "\n";
        return false;
    }
    std::string got;
    get(a, got); assert_eq(got, "OLD_OBJ", "restored obj", ok);
    if (fs::exists(b, ec0())) {
        std::cerr << "  FAILED: newly created mtl survived recovery\n";
        ok = false;
    }
    fs::remove_all(dir, ec0());
    return ok;
}

bool run_all() {
    int failed = 0;
#define T13_RUN(fn) do { \
        std::cout << "[RUN]  " << #fn << "..." << std::flush; \
        if (fn()) std::cout << " PASSED\n"; \
        else { ++failed; } \
    } while (0)
    T13_RUN(test_durable_per_entry_commit_checkpoint);
    T13_RUN(test_prepare_preserves_pending_recovery);
    T13_RUN(test_failed_recovery_preserves_evidence);
    T13_RUN(test_third_party_modification_detected);
    T13_RUN(test_crash_after_all_files_before_completion);
#undef T13_RUN
    return failed;
}

} // namespace bztests_t13
