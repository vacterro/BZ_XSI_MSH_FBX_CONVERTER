#pragma once
#include "bzconverter/core/types.hpp"
#include "bzconverter/core/json.hpp"
#include "bzconverter/core/sha256.hpp"
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
// Durable crash recovery (T-13): prepare() materializes staged bytes and
// backups of every original into an exclusively owned staging directory and
// writes a write-ahead journal. Every individual target write is bracketed by
// a durable journal checkpoint: `write-intent` BEFORE touching the target,
// `written` AFTER. Recovery never trusts volatile flags and never rolls back
// on a flag alone: staged/original SHA-256 identities prove which version a
// target holds. Unknown content fails closed with the whole transaction
// directory preserved. A new prepare refuses to start while any pending
// transaction exists, so unresolved backups can never be clobbered.
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
            std::string pending;
            if (find_pending_transaction(entries_, pending)) {
                prepared_error_ = err = "RECOVERY REQUIRED: unresolved bundle transaction " + pending +
                                        "; run 'bzconv recover <dir>' before writing outputs";
                return false;
            }
            stage_dir_ = staging_dir_for(entries_.front().path);
            std::error_code ec;
            if (!std::filesystem::create_directories(stage_dir_, ec) || ec) {
                prepared_error_ = err = "Cannot create staging directory " + utf8_from_path(stage_dir_) + ": " + ec.message();
                return false;
            }
            transaction_id_ = make_transaction_id();
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
            if (!write_journal("prepared", err)) { prepared_error_ = err; return false; }
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
            if (!write_journal_entry(e, "write-intent", err))
                return fail_with_rollback("Cannot checkpoint write intent for " + e.role + ": " + err, err);
            if (!write_file_atomic(e.path, e.staged.data(), e.staged.size(), err))
                return fail_with_rollback("Cannot write " + e.role + " " + utf8_from_path(e.path) + ": " + err, err);
            e.committed = true;
            // Durable per-file commit point. If the process dies after the
            // next write starts, recovery sees this entry as written.
            if (!write_journal_entry(e, "written", err))
                return fail_with_rollback("Bundle committed but journal update failed: " + err, err);
        }
        if (!stage_dir_.empty()) {
            if (!write_journal("committed", err))
                return fail_with_rollback("Bundle committed but journal update failed: " + err, err);
            std::error_code ec;
            std::filesystem::remove_all(stage_dir_, ec);
            stage_dir_.clear();
        }
        return true;
    }

    // Roll back every *.bzconv-stage leftover of a crashed commit in
    // `directory`. Evidence is removed only after a verified rollback.
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

        std::string state = jr["state"].as_str();
        if (state == "rolled_back") {
            acted = "already rolled back";
            std::error_code ec;
            fs::remove_all(stage_dir, ec);
            return true;
        }
        if (state == "committed" || jr["committedAll"].as_bool(false)) {
            acted = "already committed";
            std::error_code ec;
            fs::remove_all(stage_dir, ec);
            return true;
        }
        if (state == "recovery_failed") {
            acted = "RECOVERY FAILED (previous attempt); evidence preserved";
            return false;
        }

        Json::Array entries = jr["entries"].as_arr();
        bool clean = true;
        int restored = 0, removed = 0, already = 0;
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            Json& je = *it;
            // v2 journals persist a write phase per entry. `pending` entries
            // were never touched by the crashed process and must stay alone.
            std::string phase = je["phase"].as_str();
            bool legacy = jr["version"].as_int(1) < 2;
            bool touched = legacy ? je["committed"].as_bool(false)
                                  : (phase == "write-intent" || phase == "written" ||
                                     je["committed"].as_bool(false));
            if (!touched) continue;

            fs::path final_path = path_from_utf8(je["finalPath"].as_str());
            bool existed = je["existed"].as_bool(false);
            std::string staged_hash = je["stagedSha256"].as_str();
            std::string original_hash = je["originalSha256"].as_str();
            std::string detail;
            bool ok = false;
            std::error_code ec;
            bool target_exists = fs::exists(final_path, ec);

            if (existed) {
                std::vector<uint8_t> target;
                if (!target_exists) {
                    detail = "target missing before rollback of " + je["role"].as_str();
                } else if (!read_file_bytes(final_path, target, detail)) {
                    // read_file_bytes overwrote detail with its own error
                } else if (!staged_hash.empty() && !original_hash.empty()) {
                    std::string now = sha256_hex(target.data(), target.size());
                    if (now == original_hash) {
                        ok = true; ++already;             // idempotent re-run
                    } else if (now == staged_hash) {
                        std::string backup = je["originalBackup"].as_str();
                        std::vector<uint8_t> bytes;
                        if (backup.empty() || !read_file_bytes(path_from_utf8(backup), bytes, detail))
                            detail = "staged original missing for " + je["role"].as_str();
                        else
                            ok = write_file_atomic(final_path, bytes.data(), bytes.size(), detail);
                        if (ok) ++restored;
                    } else {
                        detail = "unknown content at " + utf8_from_path(final_path) +
                                 "; refusing to overwrite third-party changes";
                    }
                } else {
                    // Legacy journal without hashes: backup is the only
                    // available witness, restore it verbatim.
                    std::string backup = je["originalBackup"].as_str();
                    std::vector<uint8_t> bytes;
                    if (backup.empty() || !read_file_bytes(path_from_utf8(backup), bytes, detail))
                        detail = "staged original missing for " + je["role"].as_str();
                    else
                        ok = write_file_atomic(final_path, bytes.data(), bytes.size(), detail);
                    if (ok) ++restored;
                }
            } else {
                if (!target_exists) {
                    ok = true; ++already;
                } else if (!staged_hash.empty()) {
                    std::vector<uint8_t> target;
                    if (!read_file_bytes(final_path, target, detail)) {
                        // detail from read_file_bytes
                    } else if (sha256_hex(target.data(), target.size()) == staged_hash) {
                        fs::remove(final_path, ec);
                        ok = !fs::exists(final_path, ec);
                        if (!ok) detail = "cannot remove " + je["role"].as_str();
                        if (ok) ++removed;
                    } else {
                        detail = "unknown content at " + utf8_from_path(final_path) +
                                 "; refusing to delete third-party file";
                    }
                } else {
                    fs::remove(final_path, ec);
                    ok = !fs::exists(final_path, ec);
                    if (!ok) detail = "cannot remove " + je["role"].as_str();
                    if (ok) ++removed;
                }
            }
            if (!ok) { clean = false; acted += (acted.empty() ? "" : "; ") + detail; }
        }

        if (!clean) {
            write_recovery_failure(stage_dir, acted);
            return false;
        }
        if (acted.empty())
            acted = "restored " + std::to_string(restored) + ", removed " +
                    std::to_string(removed) + ", already-consistent " + std::to_string(already);
        // Mark rolled back BEFORE removing evidence. A crash during cleanup
        // leaves a terminal journal that the next restart simply cleans.
        std::string jerr2;
        jr["state"] = "rolled_back";
        jr["committedAll"] = false;
        if (!set_journal_state(stage_dir, jr, "rolled_back", jerr2)) {
            acted += "; cannot finalize journal: " + jerr2;
            write_recovery_failure(stage_dir, acted);
            return false;
        }
        std::error_code ec;
        fs::remove_all(stage_dir, ec);
        return true;
    }

private:
    static std::string esc_name(const std::string& name) {
        std::string out;
        for (char c : name)
            out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_') ? c : '_');
        return out.empty() ? std::string("entry") : out;
    }

    static std::string make_transaction_id() {
        static std::atomic<uint64_t> seq{0};
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        std::string id = std::to_string(now) + "_" + std::to_string(++seq);
#ifdef _WIN32
        id += "_" + std::to_string(GetCurrentProcessId());
#endif
        return id;
    }

    static std::filesystem::path staging_dir_for(const std::filesystem::path& primary) {
        std::filesystem::path parent = primary.parent_path();
        if (parent.empty()) parent = std::filesystem::path(".");
        // Unique per transaction: old evidence can never be reused by a
        // later conversion of the same primary output.
        return parent / ("." + to_lower(utf8_from_path(primary.filename())) + "." +
                         make_transaction_id() + ".bzconv-stage");
    }

    static std::string bundle_key(const std::filesystem::path& p) {
        std::error_code ec;
        auto c = std::filesystem::weakly_canonical(p, ec);
        if (ec) c = p;
        std::string s = to_lower(utf8_from_path(c));
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
    }

    static bool is_stage_dir(const std::filesystem::path& p) {
        std::string n = to_lower(utf8_from_path(p.filename()));
        return n.size() >= 13 && n.substr(n.size() - 13) == ".bzconv-stage";
    }

    static bool find_pending_transaction(const std::vector<Entry>& entries, std::string& found) {
        namespace fs = std::filesystem;
        if (entries.empty()) return false;
        fs::path parent = entries.front().path.parent_path();
        if (parent.empty()) parent = fs::path(".");
        std::error_code ec;
        for (const auto& it : fs::directory_iterator(parent, ec)) {
            if (!it.is_directory() || !is_stage_dir(it.path())) continue;
            fs::path jp = it.path() / "bundle-journal.json";
            std::vector<uint8_t> bytes;
            std::string jerr;
            bool has_journal = read_file_bytes(jp, bytes, jerr);
            std::string state;
            if (has_journal) {
                Json jr;
                std::string text(bytes.begin(), bytes.end());
                if (Json::parse(text, jr, jerr)) state = jr["state"].as_str();
            }
            if (state == "rolled_back" || state == "committed") continue;
            found = utf8_from_path(it.path());
            return true;
        }
        return false;
    }

    static void write_recovery_failure(const std::filesystem::path& stage_dir,
                                       const std::string& detail) {
        namespace fs = std::filesystem;
        Json jr;
        std::vector<uint8_t> bytes;
        std::string err;
        fs::path jp = stage_dir / "bundle-journal.json";
        if (read_file_bytes(jp, bytes, err)) {
            std::string text(bytes.begin(), bytes.end());
            if (Json::parse(text, jr, err)) {
                jr["state"] = std::string("recovery_failed");
                jr["recoveryError"] = detail;
                std::string out = jr.dump(2);
                write_file_atomic(jp, out.data(), out.size(), err);
            }
        }
        std::string report = "RECOVERY FAILED: " + detail + "\n";
        write_file_atomic(stage_dir / "recovery-error.txt", report.data(), report.size(), err);
    }

    static bool set_journal_state(const std::filesystem::path& stage_dir, Json jr,
                                  const std::string& state, std::string& err) {
        jr["state"] = state;
        std::string text = jr.dump(2);
        return write_file_atomic(stage_dir / "bundle-journal.json", text.data(), text.size(), err);
    }

    bool fail_with_rollback(std::string primary, std::string& err) {
        std::string recovery;
        bool ok = rollback(recovery);
        if (ok) {
            std::error_code ec;
            std::filesystem::remove_all(stage_dir_, ec);
            stage_dir_.clear();
        } else {
            write_recovery_failure(stage_dir_, recovery);
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

    Json journal_entry(const Entry& e, const std::string& phase) const {
        Json je = Json::object();
        je["role"] = e.role;
        je["finalPath"] = utf8_from_path(e.path);
        je["existed"] = e.existed;
        je["stagedTemp"] = utf8_from_path(e.staged_temp);
        je["originalBackup"] = e.stage_backup.empty() ? Json() : Json(utf8_from_path(e.stage_backup));
        je["stagedSha256"] = sha256_hex(e.staged.data(), e.staged.size());
        je["originalSha256"] = e.existed
            ? Json(sha256_hex(e.original.data(), e.original.size())) : Json();
        je["phase"] = phase;
        je["committed"] = (phase == "written");
        return je;
    }

    Json build_journal(const std::string& state) const {
        Json jentries = Json::array();
        for (const Entry& e : entries_)
            jentries.push_back(journal_entry(e, e.committed ? "written" : "pending"));
        Json jr = Json::object();
        jr["format"] = "bzconv-bundle-journal";
        jr["version"] = 2;
        jr["transactionId"] = transaction_id_;
        jr["state"] = state;
        jr["committedAll"] = (state == "committed");
        jr["entries"] = std::move(jentries);
        return jr;
    }

    bool write_journal(const std::string& state, std::string& err) {
        if (stage_dir_.empty()) return true;
        Json jr = build_journal(state);
        std::string text = jr.dump(2);
        return write_file_atomic(stage_dir_ / "bundle-journal.json", text.data(), text.size(), err);
    }

    // Write-ahead checkpoint for ONE entry. Rebuild the whole journal with
    // this entry's phase advanced, so a crash between target write and
    // checkpoint is resolved conservatively by recovery.
    bool write_journal_entry(const Entry& e, const std::string& phase, std::string& err) {
        if (stage_dir_.empty()) return true;
        Json jr = build_journal("committing");
        Json::Array arr = jr["entries"].as_arr();
        for (size_t i = 0; i < arr.size() && i < entries_.size(); ++i) {
            if (&entries_[i] == &e) {
                arr[i] = journal_entry(e, phase);
                break;
            }
        }
        jr["entries"] = std::move(arr);
        std::string text = jr.dump(2);
        return write_file_atomic(stage_dir_ / "bundle-journal.json", text.data(), text.size(), err);
    }

    std::vector<Entry> entries_;
    std::map<std::string, size_t> index_;
    std::string collision_;
    bool prepared_ = false;
    bool prepared_ok_ = false;
    std::string prepared_error_;
    std::filesystem::path stage_dir_;
    std::string transaction_id_;
};

} // namespace bz
