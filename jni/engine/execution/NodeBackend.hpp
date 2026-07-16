/*
 * Copyright (C) 2026 FebriCahyaa
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <optional>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

/**
 * @file NodeBackend.hpp
 * @brief The narrow device-node surface the execution engine writes through.
 *
 * Flux-owned (Category A).
 *
 * This is the only place in Flux that a device node may be mutated. It is a separate header
 * from both the engine and the sysfs implementation on purpose: the engine must depend on the
 * *contract*, not on the real filesystem, or none of the transactional logic could be tested
 * without root and a device. Equally, the sysfs backend must not have to know what a plan is.
 *
 * The write surface reports a NodeWriteResult rather than a bool because the difference between
 * "the filesystem is read-only", "permission was refused" and "the node ignored the value"
 * decides whether a capability is unavailable, unsupported, or simply failed — and a bool
 * throws exactly that away.
 */
namespace flux::execution {

/** Why a node operation did not do what was asked. */
enum class NodeError {
    Ok,
    PathNotAllowed,      ///< outside every approved root, or traversal/NUL-bearing
    NotFound,            ///< ENOENT
    NotRegularFile,      ///< a directory, device node, socket...
    SymlinkRejected,     ///< O_NOFOLLOW tripped, or lstat says symlink
    PermissionDenied,    ///< EACCES/EPERM, and permission could not be granted safely
    ReadOnlyFilesystem,  ///< EROFS
    WriteFailed,         ///< the write(2) itself failed or was short
    VerifyMismatch,      ///< write reported success but the node did not take the value
    ModeRestoreFailed,   ///< the value was written but the original mode could not be restored
};

const char *node_error_name(NodeError error);

/** The full outcome of one write, including what happened to the file mode. */
struct NodeWriteResult {
    NodeError error = NodeError::Ok;
    bool permission_adjusted = false; ///< write permission was temporarily granted
    bool mode_restored = true;        ///< the original mode was put back
    mode_t original_mode = 0;         ///< the mode actually observed, never assumed
    int errno_value = 0;

    [[nodiscard]] bool ok() const { return error == NodeError::Ok; }
};

/**
 * @brief Roots under which Flux is willing to write.
 *
 * An allowlist, not a denylist: a descriptor cannot reach the rest of the filesystem by being
 * wrong or by being crafted. Everything Flux tunes is a kernel virtual file, so the roots are
 * narrow by nature and there is no legitimate reason to widen them to a real filesystem.
 */
class PathPolicy {
public:
    /// The default approved roots. Deliberately excludes /data, /system, /vendor and anything
    /// else that is a real filesystem holding real files.
    static const std::vector<std::string> &default_roots();

    explicit PathPolicy(std::vector<std::string> roots = default_roots())
        : roots_(std::move(roots)) {}

    /// Reject empty, relative, NUL-bearing, traversal-bearing paths and anything outside the
    /// approved roots. Purely lexical: it runs before the filesystem is touched at all.
    [[nodiscard]] NodeError check(const std::string &path) const;

    [[nodiscard]] const std::vector<std::string> &roots() const { return roots_; }

private:
    std::vector<std::string> roots_;
};

/** The narrow filesystem surface the engine needs. Abstracted for host testing. */
class NodeBackend {
public:
    virtual ~NodeBackend() = default;
    [[nodiscard]] virtual bool exists(const std::string &path) const = 0;
    [[nodiscard]] virtual std::optional<std::string> read(const std::string &path) const = 0;

    /// The full-fidelity write. Implementations must restore the original file mode on both the
    /// success and the failure path, and must never report a refusal as success.
    virtual NodeWriteResult write_checked(const std::string &path, const std::string &value) = 0;

    /// Convenience for callers that genuinely only need "did it work". Not for the engine:
    /// it needs the error category to classify the capability.
    bool write(const std::string &path, const std::string &value) {
        return write_checked(path, value).ok();
    }
};

/**
 * @brief In-memory backend: usable in tests and anywhere a real sysfs is not present.
 *
 * Counts writes and permission elevations so a test can assert that a non-executable capability
 * performed *zero* of both, which is the property the whole capability gate exists to provide.
 */
class InMemoryNodeBackend : public NodeBackend {
public:
    void seed(const std::string &path, std::string value) { store_[path] = std::move(value); }

    /// Model a node that needs a permission bit added before it can be written, so the
    /// "zero chmod when not executable" assertions have something real to count.
    void seed_readonly_mode(const std::string &path, std::string value) {
        store_[path] = std::move(value);
        needs_elevation_[path] = true;
    }

    /// Force writes to a path to fail with a specific, realistic error.
    void fail_writes_to(const std::string &path, NodeError error = NodeError::WriteFailed) {
        failing_[path] = error;
    }

    /// Force writes of one *specific value* to fail. Models a node that accepted a new value and
    /// will not take the old one back — which is what makes a rollback fail rather than a write.
    void fail_writes_of_value(const std::string &path, std::string value,
                              NodeError error = NodeError::WriteFailed) {
        failing_values_[path + "\x1f" + value] = error;
    }
    void clear_failures() {
        failing_.clear();
        failing_values_.clear();
    }

    /// Model a node that silently ignores one specific value: the write returns success but the
    /// stored value does not change, so a read-back after that write does not match.
    void ignore_value(const std::string &path, std::string value) {
        ignored_[path] = std::move(value);
    }

    /// Model a node whose mode cannot be put back after the write.
    void fail_mode_restore(const std::string &path) { mode_restore_fails_[path] = true; }

    /// Simulate something outside Flux changing a node underneath us.
    void external_change(const std::string &path, std::string value) {
        store_[path] = std::move(value);
    }

    [[nodiscard]] int write_count(const std::string &path) const {
        auto it = writes_.find(path);
        return it == writes_.end() ? 0 : it->second;
    }
    [[nodiscard]] int total_writes() const { return total_writes_; }
    [[nodiscard]] int total_chmods() const { return total_chmods_; }
    void reset_counts() {
        writes_.clear();
        total_writes_ = 0;
        total_chmods_ = 0;
    }

    [[nodiscard]] bool exists(const std::string &path) const override { return store_.count(path) > 0; }

    [[nodiscard]] std::optional<std::string> read(const std::string &path) const override {
        auto it = store_.find(path);
        if (it == store_.end()) return std::nullopt;
        return it->second;
    }

    NodeWriteResult write_checked(const std::string &path, const std::string &value) override {
        NodeWriteResult result;
        result.original_mode = needs_elevation_.count(path) ? 0444 : 0644;

        if (!store_.count(path)) {
            result.error = NodeError::NotFound;
            return result;
        }

        // A refused write never counts as a write, and never elevates a permission: the real
        // backend checks writability before it touches the mode.
        if (const auto by_path = failing_.find(path); by_path != failing_.end()) {
            result.error = by_path->second;
            return result;
        }
        if (const auto by_value = failing_values_.find(path + "\x1f" + value);
            by_value != failing_values_.end()) {
            result.error = by_value->second;
            return result;
        }

        if (needs_elevation_.count(path)) {
            ++total_chmods_;
            result.permission_adjusted = true;
        }

        ++writes_[path];
        ++total_writes_;

        auto ignore = ignored_.find(path);
        const bool node_ignores_value = ignore != ignored_.end() && ignore->second == value;
        // Accepted by write(2), rejected by the node: the value simply does not land. This
        // fake deliberately still reports success, because the real backend does — it does not
        // read back, so it cannot know. Catching this is the engine's job, via the read-back
        // strategy. A fake that reported VerifyMismatch here would let the engine pass its
        // tests without ever performing the verification the real device requires.
        if (!node_ignores_value) store_[path] = value;

        if (mode_restore_fails_.count(path)) {
            result.mode_restored = false;
            result.error = NodeError::ModeRestoreFailed;
        }
        return result;
    }

private:
    std::unordered_map<std::string, std::string> store_;
    std::unordered_map<std::string, int> writes_;
    std::unordered_map<std::string, NodeError> failing_;
    std::unordered_map<std::string, NodeError> failing_values_; ///< key: path \x1f value
    std::unordered_map<std::string, std::string> ignored_;
    std::unordered_map<std::string, bool> needs_elevation_;
    std::unordered_map<std::string, bool> mode_restore_fails_;
    int total_writes_ = 0;
    int total_chmods_ = 0;
};

} // namespace flux::execution
