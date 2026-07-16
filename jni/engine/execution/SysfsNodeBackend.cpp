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

#include "SysfsNodeBackend.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace flux::execution {

const char *node_error_name(NodeError error) {
    switch (error) {
        case NodeError::Ok: return "ok";
        case NodeError::PathNotAllowed: return "path_not_allowed";
        case NodeError::NotFound: return "not_found";
        case NodeError::NotRegularFile: return "not_regular_file";
        case NodeError::SymlinkRejected: return "symlink_rejected";
        case NodeError::PermissionDenied: return "permission_denied";
        case NodeError::ReadOnlyFilesystem: return "read_only_filesystem";
        case NodeError::WriteFailed: return "write_failed";
        case NodeError::VerifyMismatch: return "verify_mismatch";
        case NodeError::ModeRestoreFailed: return "mode_restore_failed";
    }
    return "unknown";
}

// --- PathPolicy ------------------------------------------------------------

const std::vector<std::string> &PathPolicy::default_roots() {
    // Kernel virtual filesystems only. /proc/sys covers the scheduler, VM and network
    // tunables; /sys covers cpufreq, devfreq, kgsl and module parameters; /dev/stune and
    // /dev/cpuset are the cgroup mounts Android exposes for scheduling groups.
    //
    // /data, /system and /vendor are absent on purpose: nothing Flux tunes lives there, and a
    // privileged write into a real filesystem is precisely what this allowlist exists to make
    // impossible.
    static const std::vector<std::string> roots = {
        "/sys/",
        "/proc/sys/",
        "/proc/ppm/",
        "/proc/gpufreq/",
        "/proc/gpufreqv2/",
        "/proc/perfmgr/",
        "/proc/cpufreq/",
        "/dev/stune/",
        "/dev/cpuset/",
    };
    return roots;
}

NodeError PathPolicy::check(const std::string &path) const {
    if (path.empty()) return NodeError::PathNotAllowed;
    if (path.front() != '/') return NodeError::PathNotAllowed;
    if (path.find('\0') != std::string::npos) return NodeError::PathNotAllowed;

    // Reject traversal lexically, before any syscall. "/sys/../data/x" must never reach
    // open(2), even though the kernel would resolve it happily.
    if (path.find("/../") != std::string::npos) return NodeError::PathNotAllowed;
    if (path.size() >= 3 && path.compare(path.size() - 3, 3, "/..") == 0) {
        return NodeError::PathNotAllowed;
    }
    if (path.find("//") != std::string::npos) return NodeError::PathNotAllowed;

    for (const auto &root : roots_) {
        if (path.size() > root.size() && path.compare(0, root.size(), root) == 0) {
            return NodeError::Ok;
        }
    }
    return NodeError::PathNotAllowed;
}

// --- SysfsNodeBackend ------------------------------------------------------

int SysfsNodeBackend::open_node(const std::string &path, int flags, NodeError &error,
                                mode_t &mode) const {
    error = policy_.check(path);
    if (error != NodeError::Ok) return -1;

    // O_NOFOLLOW: refuse to follow a final symlink. Combined with the regular-file check
    // below, a swapped path cannot redirect a privileged write somewhere else.
    const int fd = ::open(path.c_str(), flags | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        switch (errno) {
            case ENOENT: error = NodeError::NotFound; break;
            case ELOOP: error = NodeError::SymlinkRejected; break;
            case EACCES:
            case EPERM: error = NodeError::PermissionDenied; break;
            case EROFS: error = NodeError::ReadOnlyFilesystem; break;
            // A directory opened for writing fails here rather than at the fstat check below,
            // so it has to be named here too — reporting it as "not found" would send whoever
            // reads the diagnostics looking for a missing node that is actually present and
            // simply the wrong kind of thing.
            case EISDIR: error = NodeError::NotRegularFile; break;
            default: error = NodeError::NotFound; break;
        }
        return -1;
    }

    // Stat the descriptor, not the name: whatever is inspected here is exactly what is
    // written, with no window in between.
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        error = NodeError::NotFound;
        return -1;
    }
    if (S_ISLNK(st.st_mode)) {
        ::close(fd);
        error = NodeError::SymlinkRejected;
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        // sysfs/procfs attributes are regular files. A directory or device node here means the
        // descriptor is wrong, and writing it could do something entirely unintended.
        ::close(fd);
        error = NodeError::NotRegularFile;
        return -1;
    }

    mode = st.st_mode & 07777; // the real mode, never an assumed 0444
    error = NodeError::Ok;
    return fd;
}

bool SysfsNodeBackend::exists(const std::string &path) const {
    if (policy_.check(path) != NodeError::Ok) return false;
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) return false;
    // A symlink does not count as existing: Flux will refuse to write it later anyway, and
    // reporting it as present would make a capability look supported when it is not.
    return S_ISREG(st.st_mode);
}

std::optional<std::string> SysfsNodeBackend::read(const std::string &path) const {
    NodeError error = NodeError::Ok;
    mode_t mode = 0;
    const int fd = open_node(path, O_RDONLY, error, mode);
    if (fd < 0) return std::nullopt;

    std::string out;
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return std::nullopt;
        }
        if (n == 0) break;
        out.append(buffer, static_cast<size_t>(n));
        if (out.size() > (1u << 20)) break; // a tuning node is never a megabyte; stop reading
    }
    ::close(fd);

    // Kernel attributes are newline-terminated; callers compare against bare values.
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

NodeError SysfsNodeBackend::probe_writable(const std::string &path) const {
    NodeError error = NodeError::Ok;
    mode_t mode = 0;

    // Try read-only first: it answers "does this exist, is it a regular file, is it under an
    // approved root" without needing any permission at all.
    int fd = open_node(path, O_RDONLY, error, mode);
    if (fd < 0) return error;
    ::close(fd);

    if (mode & S_IWUSR) return NodeError::Ok; // already writable by owner

    // Not writable as it stands. It may still be adjustable, but that is only knowable by
    // trying, and probing must not mutate the device. Report it as permission-denied and let
    // the write path decide; a capability that turns out to be unadjustable will surface then.
    return NodeError::PermissionDenied;
}

NodeWriteResult SysfsNodeBackend::write_checked(const std::string &path, const std::string &value) {
    NodeWriteResult result;

    NodeError error = NodeError::Ok;
    mode_t mode = 0;
    // O_TRUNC, because a shorter value must replace a longer one rather than overwrite its
    // prefix. Without it, writing "schedutil" over "performance" leaves "schedutilce" behind.
    // Real sysfs store handlers only see this single write(2) and so are unaffected, but the
    // backend deliberately accepts any regular file under an approved root (cgroup and procfs
    // nodes included), and on those the leftover tail is real. This is also what the shell's
    // `>` redirect does, so it is not a behaviour change from the legacy applier.
    int fd = open_node(path, O_WRONLY | O_TRUNC, error, mode);

    if (fd < 0 && (error == NodeError::PermissionDenied)) {
        // Opening for write was refused. Reopen read-only to learn the real mode, then grant
        // the minimum permission on that descriptor.
        NodeError ro_error = NodeError::Ok;
        const int ro_fd = open_node(path, O_RDONLY, ro_error, mode);
        if (ro_fd < 0) {
            result.error = ro_error;
            result.errno_value = errno;
            last_error_ = result.error;
            return result;
        }
        result.original_mode = mode;

        // Add only S_IWUSR. The legacy script set a blanket 0644, which also granted group and
        // other read access the node may never have had.
        if (::fchmod(ro_fd, mode | S_IWUSR) != 0) {
            const int saved = errno;
            ::close(ro_fd);
            result.error = (saved == EROFS) ? NodeError::ReadOnlyFilesystem
                                            : NodeError::PermissionDenied;
            result.errno_value = saved;
            last_error_ = result.error;
            return result;
        }
        ::close(ro_fd);
        result.permission_adjusted = true;

        fd = open_node(path, O_WRONLY | O_TRUNC, error, mode);
        if (fd < 0) {
            // Could not write even after granting permission. Put the mode back before
            // leaving: a node left writable is a lasting change to the device.
            NodeError restore_error = NodeError::Ok;
            mode_t ignored = 0;
            const int restore_fd = open_node(path, O_RDONLY, restore_error, ignored);
            if (restore_fd >= 0) {
                if (::fchmod(restore_fd, result.original_mode) != 0) result.mode_restored = false;
                ::close(restore_fd);
            } else {
                result.mode_restored = false;
            }
            result.error = error;
            last_error_ = result.error;
            return result;
        }
    } else if (fd < 0) {
        result.error = error;
        result.errno_value = errno;
        last_error_ = result.error;
        return result;
    } else {
        result.original_mode = mode;
    }

    // --- the write itself ---
    ssize_t written = 0;
    for (;;) {
        written = ::write(fd, value.data(), value.size());
        if (written < 0 && errno == EINTR) continue;
        break;
    }
    const int write_errno = errno;
    ::close(fd);

    if (written < 0 || static_cast<size_t>(written) != value.size()) {
        switch (write_errno) {
            case EACCES:
            case EPERM: result.error = NodeError::PermissionDenied; break;
            case EROFS: result.error = NodeError::ReadOnlyFilesystem; break;
            default: result.error = NodeError::WriteFailed; break;
        }
        result.errno_value = write_errno;
    }

    // --- restore the exact original mode, success or failure ---
    if (result.permission_adjusted) {
        NodeError restore_error = NodeError::Ok;
        mode_t ignored = 0;
        const int restore_fd = open_node(path, O_RDONLY, restore_error, ignored);
        if (restore_fd >= 0) {
            if (::fchmod(restore_fd, result.original_mode) != 0) result.mode_restored = false;
            ::close(restore_fd);
        } else {
            result.mode_restored = false;
        }
    }

    // A value written into a node whose permissions Flux could not put back is still a device
    // left in a state Flux did not find it in. Say so rather than reporting a clean success.
    if (result.ok() && !result.mode_restored) result.error = NodeError::ModeRestoreFailed;

    last_error_ = result.error;
    return result;
}

} // namespace flux::execution
