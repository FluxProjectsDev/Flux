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
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "ExecutionEngine.hpp"

/**
 * @file SysfsNodeBackend.hpp
 * @brief The real filesystem backend: the single place in Flux that writes a device node.
 *
 * ## Provenance
 *
 * Flux-owned (Category A). Designed from documented Linux/POSIX filesystem semantics —
 * open(2), lstat(2), fchmod(2), O_NOFOLLOW, and the errno contract — not by translating the
 * legacy shell applier. The legacy script wrapped every write in
 * `chmod 644 -> echo -> chmod 444`; that sequence is *not* reproduced here, and its two
 * defects are deliberately not inherited:
 *
 *  - it assumed the original mode was 0444 and hard-restored that, so a node that was really
 *    0644 came back wrong;
 *  - it ran chmod through the shell on an interpolated path, and discarded every error, so a
 *    node that could not be written looked identical to one that was.
 *
 * This backend records the node's *actual* mode, adds only the one permission bit it needs,
 * and puts the exact original mode back — on the failure path too.
 *
 * ## Why permissions are touched at all
 *
 * Many vendor tuning nodes ship root-owned and read-only (commonly 0444). Writing them
 * requires temporarily granting write permission. That is a genuinely dangerous operation, so
 * it is confined:
 *
 *  - the target must resolve under an explicitly approved procfs/sysfs root;
 *  - the path is opened with O_NOFOLLOW and must be a regular file — a symlink or a device
 *    node is refused, so a swapped path cannot redirect a privileged write;
 *  - permission is granted on the open file descriptor (fchmod), never by name, so the file
 *    cannot be exchanged between the check and the write;
 *  - only S_IWUSR is added, never a blanket 0644;
 *  - the original mode is restored whether the write succeeds or fails.
 *
 * ## Errors are never success
 *
 * EACCES, EROFS, ENOENT and a read-back mismatch are distinct, reported outcomes. A node that
 * cannot be written must make its capability unavailable — it must never let Flux claim a
 * profile is active.
 */
namespace flux::execution {

/** Why a node operation ended the way it did. Never collapse these into a bool. */
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

/** The outcome of one node write, including whether permissions had to be changed. */
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

/**
 * @brief The production NodeBackend. The only component in Flux that writes a device node.
 *
 * Satisfies NodeBackend so the engine, planner and every host test can drive it through the
 * same interface as the in-memory fake.
 */
class SysfsNodeBackend : public NodeBackend {
public:
    explicit SysfsNodeBackend(PathPolicy policy = PathPolicy{}) : policy_(std::move(policy)) {}

    [[nodiscard]] bool exists(const std::string &path) const override;
    [[nodiscard]] std::optional<std::string> read(const std::string &path) const override;

    /// Convenience for the NodeBackend contract. Prefer write_checked(): a bare bool cannot
    /// distinguish "read-only filesystem" from "node ignored the value", and the difference
    /// decides whether a capability is unavailable or a write simply failed.
    bool write(const std::string &path, const std::string &value) override;

    /// The full-fidelity write: permission handling, exact mode restoration, real errno.
    NodeWriteResult write_checked(const std::string &path, const std::string &value);

    /// Can this node be written, either already or by safely adding one permission bit?
    /// Answers without writing, for capability probing.
    [[nodiscard]] NodeError probe_writable(const std::string &path) const;

    [[nodiscard]] NodeError last_error() const { return last_error_; }

private:
    PathPolicy policy_;
    NodeError last_error_ = NodeError::Ok;

    /// Open a validated, non-symlink, regular file under an approved root. Returns -1 and sets
    /// @p error otherwise.
    int open_node(const std::string &path, int flags, NodeError &error, mode_t &mode) const;
};

} // namespace flux::execution
