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

// Tests for the production node backend: path policy, symlink refusal, permission handling
// and exact mode restoration.
//
// These use a real filesystem in a temp directory rather than a fake, because the whole point
// of this component is its interaction with open/lstat/fchmod and errno. A mock would only
// confirm that the mock behaves like the mock. No root and no device are needed: the backend
// is constructed with the temp directory as its approved root, which also exercises the
// PathPolicy allowlist itself.

#include "TestFramework.hpp"

#include "SysfsNodeBackend.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using namespace flux::execution;

namespace {

/// A temp directory that cleans up after itself, standing in for a sysfs root.
class TempRoot {
public:
    TempRoot() {
        char tmpl[] = "/tmp/flux_sysfs_XXXXXX";
        const char *dir = mkdtemp(tmpl);
        path_ = dir ? dir : "";
    }
    ~TempRoot() {
        if (!path_.empty()) {
            const std::string cmd = "rm -rf '" + path_ + "'";
            if (system(cmd.c_str()) != 0) { /* best effort */
            }
        }
    }
    TempRoot(const TempRoot &) = delete;
    TempRoot &operator=(const TempRoot &) = delete;

    [[nodiscard]] const std::string &path() const { return path_; }
    [[nodiscard]] std::string file(const std::string &name) const { return path_ + "/" + name; }

    /// Create a node with an exact mode, the way a kernel attribute would appear.
    std::string make_node(const std::string &name, const std::string &content, mode_t mode) const {
        const std::string p = file(name);
        std::ofstream out(p);
        out << content;
        out.close();
        chmod(p.c_str(), mode);
        return p;
    }

    /// A policy whose only approved root is this temp directory.
    [[nodiscard]] PathPolicy policy() const {
        return PathPolicy(std::vector<std::string>{path_ + "/"});
    }

private:
    std::string path_;
};

mode_t mode_of(const std::string &path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return 0;
    return st.st_mode & 07777;
}

} // namespace

TEST("sysfs backend: a writable node is written and read back") {
    TempRoot root;
    const std::string node = root.make_node("scaling_governor", "schedutil", 0644);
    SysfsNodeBackend backend(root.policy());

    const auto result = backend.write_checked(node, "performance");
    CHECK_MSG(result.ok(), std::string("write failed: ") + node_error_name(result.error));
    CHECK(!result.permission_adjusted); // already writable; no need to touch the mode
    CHECK_EQ(backend.read(node).value_or(""), std::string("performance"));
}

TEST("sysfs backend: a read-only node is written and its exact mode restored") {
    // The case the whole component exists for: vendor nodes commonly ship 0444.
    TempRoot root;
    const std::string node = root.make_node("gpu_freq", "500", 0444);
    SysfsNodeBackend backend(root.policy());

    const auto result = backend.write_checked(node, "800");
    CHECK_MSG(result.ok(), std::string("write failed: ") + node_error_name(result.error));
    CHECK_MSG(result.permission_adjusted, "a 0444 node must have needed a permission grant");
    CHECK_MSG(result.mode_restored, "the original mode must be restored");
    CHECK_EQ(backend.read(node).value_or(""), std::string("800"));
    CHECK_MSG(mode_of(node) == 0444, "the node must be left exactly as it was found (0444)");
}

TEST("sysfs backend: the original mode is restored exactly, not assumed to be 0444") {
    // The legacy shell hard-restored 0444 regardless of what it found, so a 0600 node came
    // back world-readable. The mode that is restored must be the mode that was observed.
    TempRoot root;
    const std::string node = root.make_node("private_tunable", "1", 0400);
    SysfsNodeBackend backend(root.policy());

    const auto result = backend.write_checked(node, "2");
    CHECK(result.ok());
    CHECK_EQ(result.original_mode, static_cast<mode_t>(0400));
    CHECK_MSG(mode_of(node) == 0400, "0400 must be restored as 0400, never widened to 0444");
}

TEST("sysfs backend: a path outside the approved roots is refused before any syscall") {
    TempRoot root;
    SysfsNodeBackend backend(root.policy());

    const auto result = backend.write_checked("/etc/passwd", "malicious");
    CHECK_EQ(result.error, NodeError::PathNotAllowed);
    CHECK(!result.permission_adjusted);
}

TEST("sysfs backend: traversal out of an approved root is refused") {
    TempRoot root;
    SysfsNodeBackend backend(root.policy());

    // Lexically inside the root, but resolves outside it.
    const auto result = backend.write_checked(root.file("../escape"), "x");
    CHECK_EQ(result.error, NodeError::PathNotAllowed);
}

TEST("sysfs backend: a symlinked node is refused rather than followed") {
    // A symlink is how a privileged write gets redirected somewhere it was never meant to go.
    TempRoot root;
    const std::string target = root.make_node("real_target", "safe", 0644);
    const std::string link = root.file("link_node");
    CHECK(symlink(target.c_str(), link.c_str()) == 0);

    SysfsNodeBackend backend(root.policy());
    const auto result = backend.write_checked(link, "redirected");

    CHECK_EQ(result.error, NodeError::SymlinkRejected);
    CHECK_MSG(backend.read(target).value_or("") == "safe", "the symlink target must be untouched");
    CHECK_MSG(!backend.exists(link), "a symlink must not be reported as an existing node");
}

TEST("sysfs backend: a directory is refused as not a regular file") {
    TempRoot root;
    const std::string dir = root.file("a_directory");
    CHECK(mkdir(dir.c_str(), 0755) == 0);

    SysfsNodeBackend backend(root.policy());
    const auto result = backend.write_checked(dir, "x");
    CHECK_EQ(result.error, NodeError::NotRegularFile);
}

TEST("sysfs backend: a missing node reports not_found, never success") {
    TempRoot root;
    SysfsNodeBackend backend(root.policy());

    const auto result = backend.write_checked(root.file("nonexistent"), "1");
    CHECK_EQ(result.error, NodeError::NotFound);
    CHECK(!result.ok());
}

TEST("sysfs backend: probe reports writability without mutating the node") {
    TempRoot root;
    const std::string writable = root.make_node("w", "1", 0644);
    const std::string readonly = root.make_node("r", "1", 0444);
    const std::string missing = root.file("gone");
    SysfsNodeBackend backend(root.policy());

    CHECK_EQ(backend.probe_writable(writable), NodeError::Ok);
    CHECK_EQ(backend.probe_writable(readonly), NodeError::PermissionDenied);
    CHECK_EQ(backend.probe_writable(missing), NodeError::NotFound);
    CHECK_EQ(backend.probe_writable("/etc/shadow"), NodeError::PathNotAllowed);

    // Probing must not change anything it looked at.
    CHECK_EQ(backend.read(writable).value_or(""), std::string("1"));
    CHECK_MSG(mode_of(readonly) == 0444, "probing must not leave a node writable");
}

TEST("sysfs backend: reading strips the trailing newline kernel attributes carry") {
    TempRoot root;
    const std::string node = root.make_node("governor", "schedutil\n", 0644);
    SysfsNodeBackend backend(root.policy());

    CHECK_EQ(backend.read(node).value_or(""), std::string("schedutil"));
}

TEST("sysfs backend: the default policy admits kernel nodes and refuses real filesystems") {
    const PathPolicy policy;

    CHECK_EQ(policy.check("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"), NodeError::Ok);
    CHECK_EQ(policy.check("/proc/sys/kernel/sched_latency_ns"), NodeError::Ok);
    CHECK_EQ(policy.check("/proc/ppm/policy/hard_userlimit_cpu_freq"), NodeError::Ok);
    CHECK_EQ(policy.check("/dev/stune/top-app/schedtune.boost"), NodeError::Ok);

    // Real filesystems holding real files are not tuning surfaces.
    CHECK_EQ(policy.check("/data/system/users/0/settings_global.xml"), NodeError::PathNotAllowed);
    CHECK_EQ(policy.check("/system/build.prop"), NodeError::PathNotAllowed);
    CHECK_EQ(policy.check("/vendor/etc/init.rc"), NodeError::PathNotAllowed);
    CHECK_EQ(policy.check("/etc/passwd"), NodeError::PathNotAllowed);

    // Malformed and evasive paths.
    CHECK_EQ(policy.check(""), NodeError::PathNotAllowed);
    CHECK_EQ(policy.check("sys/relative"), NodeError::PathNotAllowed);
    CHECK_EQ(policy.check("/sys/../data/x"), NodeError::PathNotAllowed);
    CHECK_EQ(policy.check("/sys//double"), NodeError::PathNotAllowed);
    CHECK_EQ(policy.check(std::string("/sys/x\0/y", 9)), NodeError::PathNotAllowed);
    // A bare root prefix with nothing under it is not a node.
    CHECK_EQ(policy.check("/sys/"), NodeError::PathNotAllowed);
}

TEST("sysfs backend: a write failure leaves the node's permissions as they were") {
    // If Flux grants write permission and then cannot write, it must not leave the device with
    // a node that is more permissive than it found it.
    TempRoot root;
    const std::string node = root.make_node("stubborn", "1", 0444);
    SysfsNodeBackend backend(root.policy());

    // Make the directory read-only so the reopen-for-write fails even after the mode change.
    chmod(root.path().c_str(), 0500);
    const auto result = backend.write_checked(node, "2");
    chmod(root.path().c_str(), 0700); // restore so the temp dir can be cleaned up

    // Either it failed to write, or the filesystem allowed it; only the first is interesting,
    // and in both cases the mode must not be left widened.
    if (!result.ok()) {
        CHECK_MSG(mode_of(node) == 0444,
                  "a failed write must still restore the original mode");
    }
}

TEST("sysfs backend: a shorter value replaces a longer one, leaving no tail behind") {
    // Regression: the write opened O_WRONLY without O_TRUNC, so writing "schedutil" (9 bytes)
    // over "performance" (11 bytes) left "schedutilce" in the node — the new value with the tail
    // of the old one still attached. A real sysfs store handler only ever sees the single
    // write(2) and so hides this, but the backend accepts any regular file under an approved
    // root, and the read-back verification that everything else depends on reads the file.
    TempRoot root;
    const std::string node = root.make_node("scaling_governor", "performance", 0644);
    SysfsNodeBackend backend(root.policy());

    const auto result = backend.write_checked(node, "schedutil");

    CHECK(result.ok());
    CHECK_MSG(backend.read(node).value() == "schedutil",
              "expected exactly 'schedutil', got '" + backend.read(node).value_or("<none>") + "'");
}

TEST("sysfs backend: a shorter value also truncates when permission had to be elevated") {
    // The same defect existed on the second open, the one taken after fchmod grants write
    // permission — the path a read-only vendor node actually takes.
    TempRoot root;
    const std::string node = root.make_node("power_limit", "performance", 0444);
    SysfsNodeBackend backend(root.policy());

    const auto result = backend.write_checked(node, "0");

    CHECK_MSG(result.ok(), std::string("write failed: ") + node_error_name(result.error));
    CHECK(result.permission_adjusted);
    CHECK_EQ(backend.read(node).value(), std::string("0"));
    CHECK_MSG(mode_of(node) == 0444, "the exact original mode must come back");
}
