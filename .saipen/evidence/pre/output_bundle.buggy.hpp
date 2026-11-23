#pragma once
#include "bzconverter/core/types.hpp"
#include <map>
#include <utility>

namespace bz {

// A multi-file export declared as one unit. Exporters stage every artifact
// (including sidecars and textures) before any file is touched, then the
// bundle preflights all targets and commits with in-memory rollback. This
// replaces the old per-file atomic-write safety, which could overwrite a
// sidecar that the pipeline never checked.
class OutputBundle {
public:
    struct Entry {
        std::filesystem::path path;
        std::vector<uint8_t> staged;
        std::vector<uint8_t> original;
        std::string role;
        bool existed = false;
        bool committed = false;
    };

    void add(const std::filesystem::path& path, std::vector<uint8_t> bytes, std::string role) {
        if (prepared_) { // Never mutate a bundle after preflight.
            return;
        }
        std::string key = bundle_key(path);
        auto it = index_.find(key);
        if (it != index_.end()) {
            Entry& e = entries_[it->second];
            if (e.staged != bytes) {
                collision_ = "conflicting staged content for " + e.role + " and " + role;
            }
            return;
        }
        index_[key] = entries_.size();
        entries_.push_back({path, std::move(bytes), {}, std::move(role), false, false});
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
        prepared_ok_ = true;
        return true;
    }

    bool commit(bool overwrite, std::atomic<bool>* cancel_flag, std::string& err) {
        if (!prepare(overwrite, err)) return false;
        if (cancel_flag && cancel_flag->load()) {
            return fail_with_rollback("CANCELLED_BEFORE_COMMIT", err);
        }
        for (Entry& e : entries_) {
            if (cancel_flag && cancel_flag->load()) {
                return fail_with_rollback("CANCELLED_DURING_COMMIT", err);
            }
            if (!write_file_atomic(e.path, e.staged.data(), e.staged.size(), err)) {
                return fail_with_rollback("Cannot write " + e.role + " " + utf8_from_path(e.path) + ": " + err, err);
            }
            e.committed = true;
        }
        return true;
    }

private:
    static std::string bundle_key(const std::filesystem::path& p) {
        std::error_code ec;
        auto c = std::filesystem::weakly_canonical(p, ec);
        if (ec) c = p;
        std::string s = to_lower(utf8_from_path(c));
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
    }

    bool fail_with_rollback(std::string primary, std::string& err) {
        (void)primary;
        rollback(err);
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
                ok = !ec || !std::filesystem::exists(e.path, ec);
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

    std::vector<Entry> entries_;
    std::map<std::string, size_t> index_;
    std::string collision_;
    bool prepared_ = false;
    bool prepared_ok_ = false;
    std::string prepared_error_;
};

} // namespace bz
