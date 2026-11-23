#pragma once
#include "bzconverter/core/types.hpp"
#include "bzconverter/core/json.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace bz {

// A multi-file export declared as one unit. Exporters stage every artifact
// (including sidecars and textures) before any file is touched, then the
// bundle preflights all targets and commits with in-memory rollback.
//
// Durable crash recovery (SAIHANDOFF-12 §12): prepare() materializes staged
// bytes and backups of every original into a staging directory next to the
// primary output and records the plan in bundle-journal.json. Commit marks
// the journal once every file has been placed. If the process dies
// mid-commit, journal + backups roll the whole bundle back without the
// process: recover_pending().
class OutputBundle {
public:
    struct Entry {
        std::filesystem::path path;
        std::vector<uint8_t> staged;
        std::vector<uint8_t> original;
        std::string role;
        bool existed = false;
        bool committed = false;
        std::filesystem::path staged_temp;
        std::filesystem::path stage_backup;
    };

    void add(const std::filesystem::path& path, std::vector<uint8_t> bytes, std::string role) {
        if (prepared_) return;
        std::string key = bundle_key(path);
        auto it = index_.find(key);
        if (it != index_.end()) {
            Entry& e = entries_[it->second];
            if (e.staged != bytes)
                collision_ = "conflicting staged content for " + e.role + " and " + role;
            return;
        }
        index_[key] = entries_.size();
        entries_.push_back({path, std::move(bytes), {}, std::move(role), false, false, {}, {}});
    }

    const std::vector<Entry>& entries() const { return entries_; }
    bool empty() const { return entries_.empty(); }
    const std::string& collision() const { return collision_; }

    bool prepare(bool overwrite, std::string& err) {
        if (prepared_) { err = prepared_error_; return prepared_ok_; }
        prepared_ = true;
        if (!collision_.empty()) { err = collision_; prepared_error_ = err; return false; }
        for (Entry& e : entries_) {
            std::error_code ec;
            e.existed = std::filesystem::exists(e.path, ec);
            if (!e.existed) continue;
            if (!overwrite) {
                err = "Output already exists; overwrite disabled: " + e.role + " " + utf8_from_path(e.path);
                prepared_error_ = err;
                return false;
            }
            if (!read_file_bytes(e.path, e.original, err)) {
                err = "Cannot read existing " + e.role + " for rollback: " + utf8_from_path(e.path) + ": " + err;
                prepared_error_ = err;
                return false;
            }
        }
        if (!entries_.empty()) {
            stage_dir_ = staging_dir_for(entries_.front().path);
            std::error_code ec;
            std::filesystem::remove_all(stage_dir_, ec);
            if (!std::filesystem::create_directories(stage_dir_, ec) && ec) {
                prepared_error_ = err = "Cannot create staging directory " + utf8_from_path(stage_dir_) + ": " + ec.message();
                return false;
            }
            for (size_t i = 0; i < entries_.size(); ++i) {
                Entry& e = entries_[i];
                std::string name = esc_name(utf8_from_path(e.path.filename()));
                std::filesystem::path staged_path = stage_dir_ / (std::to_string(i) + "_" + name);
                if (!write_file_atomic(staged_path, e.staged.data(), e.staged.size(), err)) {
                    prepared_error_ = err = "Cannot stage " + e.role + ": " + err;
                    return false;
                }
                if (e.existed) {
                    std::filesystem::path orig_path = stage_dir_ / (std::to_string(i) + "_orig_" + name);
                    if (!write_file_atomic(orig_path, e.original.data(), e.original.size(), err)) {
                        prepared_error_ = err = "Cannot back up " + e.role + ": " + err;
                        return false;
                    }
                    e.stage_backup = orig_path;
                }
                e.staged_temp = staged_path;
            }
            if (!write_journal(false, err)) { prepared_error_ = err; return false; }
        }
        prepared_ok_ = true;
        return true;
    }

    bool commit(bool overwrite, std::atomic<bool>* cancel_flag, std::string& err) {
        if (!prepare(overwrite, err)) return false;
        if (cancel_flag && cancel_flag->load())
            return fail_with_rollback("CANCELLED_BEFORE_COMMIT", err);
        for (Entry& e : entries_) {
            if (cancel_flag && cancel_flag->load())
                return fail_with_rollback("CANCELLED_DURING_COMMIT", err);
            if (!write_file_atomic(e.path, e.staged.data(), e.staged.size(), err))
                return fail_with_rollback("Cannot write " + e.role + " " + utf8_from_path(e.path) + ": " + err, err);
            e.committed = true;
        }
        if (!stage_dir_.empty()) {
            std::string jerr;
            if (!write_journal(true, jerr)) {
                fail_with_rollback("Bundle committed but journal update failed: " + jerr, err);
                return false;
            }
            std::error_code ec;
            std::filesystem::remove_all(stage_dir_, ec);
            stage_dir_.clear();
        }
        return true;
    }

    // §12: roll back every *.bzconv-stage leftover of a crashed commit in
    // `directory` (restore originals, remove newly created outputs).
    static bool recover_pending(const std::filesystem::path& directory, std::string& report_out) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(directory, ec)) return true;
        bool clean_all = true;
        for (const auto& it : fs::directory_iterator(directory, ec)) {
            if (!it.is_directory()) continue;
            std::string name = to_lower(utf8_from_path(it.path().filename()));
            if (name.size() < 13 || name.substr(name.size() - 13) != ".bzconv-stage") continue;
            std::string acted;
            bool ok = recover_one(it.path(), acted);
            report_out += (report_out.empty() ? "" : "; ") + utf8_from_path(it.path()) + ": " +
                          (ok ? "recovered" : "RECOVERY FAILED") +
                          (acted.empty() ? "" : " (" + acted + ")");
            clean_all = clean_all && ok;
        }
        return clean_all;
    }

    // Recovery of a partially committed bundle, driven only by the durable
    // journal + staged originals. No in-memory state from the crashed process.
    static bool recover_one(const std::filesystem::path& stage_dir, std::string& acted) {
        namespace fs = std::filesystem;
        fs::path jpath = stage_dir / "bundle-journal.json";
        std::vector<uint8_t> jbytes;
        std::string jerr;
        if (!read_file_bytes(jpath, jbytes, jerr)) { acted = "journal unreadable: " + jerr; return false; }
        Json jr;
        std::string jtext(jbytes.begin(), jbytes.end());
        if (!Json::parse(jtext, jr, jerr)) { acted = "journal invalid: " + jerr; return false; }
        if (jr["committedAll"].as_bool(false)) {
            acted = "already committed";
            std::error_code ec;
            fs::remove_all(stage_dir, ec);
            return true;
        }
        Json::Array entries = jr["entries"].as_arr();
        bool clean = true;
        int restored = 0, removed = 0;
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            Json& je = *it;
            if (!je["committed"].as_bool(false)) continue;
            fs::path final_path = path_from_utf8(je["finalPath"].as_str());
            bool existed = je["existed"].as_bool(false);
            bool ok = false;
            std::string detail;
            if (existed) {
                std::string backup = je["originalBackup"].as_str();
                std::vector<uint8_t> bytes;
                if (backup.empty() || !read_file_bytes(path_from_utf8(backup), bytes, detail))
                    detail = "staged original missing for " + je["role"].as_str();
                else
                    ok = write_file_atomic(final_path, bytes.data(), bytes.size(), detail);
                if (ok) ++restored;
            } else {
                std::error_code ec;
                fs::remove(final_path, ec);
                ok = !fs::exists(final_path, ec);
                if (!ok) detail = "cannot remove " + je["role"].as_str();
                if (ok) ++removed;
            }
            if (!ok) { clean = false; acted += (acted.empty() ? "" : "; ") + detail; }
        }
        if (acted.empty()) acted = "restored " + std::to_string(restored) + ", removed " + std::to_string(removed);
        std::error_code ec;
        fs::remove_all(stage_dir, ec);
        return clean;
    }

private:
    static std::string esc_name(const std::string& name) {
        std::string out;
        for (char c : name)
            out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_') ? c : '_');
        return out.empty() ? std::string("entry") : out;
    }

    static std::filesystem::path staging_dir_for(const std::filesystem::path& primary) {
        std::filesystem::path parent = primary.parent_path();
        if (parent.empty()) parent = std::filesystem::path(".");
        return parent / ("." + to_lower(utf8_from_path(primary.filename())) + ".bzconv-stage");
    }

    static std::string bundle_key(const std::filesystem::path& p) {
        std::error_code ec;
        auto c = std::filesystem::weakly_canonical(p, ec);
        if (ec) c = p;
        std::string s = to_lower(utf8_from_path(c));
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
    }

    bool fail_with_rollback(std::string primary, std::string& err) {
        std::string recovery;
        bool ok = rollback(recovery);
        if (ok) {
            std::error_code ec;
            std::filesystem::remove_all(stage_dir_, ec);
            stage_dir_.clear();
        }
        err = std::move(primary);
        if (!ok) err += " | ROLLBACK FAILED: " + recovery;
        return false;
    }

    bool rollback(std::string& recovery) {
        bool clean = true;
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
            Entry& e = *it;
            if (!e.committed) continue;
            std::string detail;
            bool ok = false;
            if (e.existed) {
                ok = write_file_atomic(e.path, e.original.data(), e.original.size(), detail);
                if (!ok) detail = "Cannot restore " + e.role + ": " + detail;
            } else {
                std::error_code ec;
                std::filesystem::remove(e.path, ec);
                ok = !std::filesystem::exists(e.path, ec);
                if (!ok) detail = "Cannot remove rolled-back " + e.role + ": " + ec.message();
            }
            if (!ok) {
                clean = false;
                if (!detail.empty()) recovery += (recovery.empty() ? "" : "; ") + detail;
            }
            e.committed = false;
        }
        return clean;
    }

    bool write_journal(bool done, std::string& err) {
        if (stage_dir_.empty()) return true;
        Json jentries = Json::array();
        for (const Entry& e : entries_) {
            Json je = Json::object();
            je["role"] = e.role;
            je["finalPath"] = utf8_from_path(e.path);
            je["existed"] = e.existed;
            je["stagedTemp"] = utf8_from_path(e.staged_temp);
            je["originalBackup"] = e.stage_backup.empty() ? Json() : Json(utf8_from_path(e.stage_backup));
            je["committed"] = e.committed;
            jentries.push_back(std::move(je));
        }
        Json jr = Json::object();
        jr["format"] = std::string("bzconv-bundle-journal");
        jr["version"] = 1;
        jr["committedAll"] = done;
        jr["entries"] = std::move(jentries);
        std::filesystem::path jpath = stage_dir_ / "bundle-journal.json";
        std::string text = jr.dump(2);
        return write_file_atomic(jpath, text.data(), text.size(), err);
    }

    std::vector<Entry> entries_;
    std::map<std::string, size_t> index_;
    std::string collision_;
    bool prepared_ = false;
    bool prepared_ok_ = false;
    std::string prepared_error_;
    std::filesystem::path stage_dir_;
};

} // namespace bz
