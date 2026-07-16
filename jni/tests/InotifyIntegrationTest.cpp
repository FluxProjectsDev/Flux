/*
 * Copyright (C) 2024-2026 FebriCahyaa
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

#include "TestFramework.hpp"

#include <InotifyEvents.hpp>
#include <InotifyWatcher.hpp>
#include <SynthesisCore.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

/**
 * @file InotifyIntegrationTest.cpp
 * @brief End-to-end test of the SynthesisCore -> Flux update path.
 *
 * inotify is a Linux kernel facility, so this runs against the *real* kernel with the *real*
 * InotifyWatcher — not a mock. It reproduces exactly what SynthesisCore's AtomicStatusWriter
 * does (write `<file>.tmp`, fsync, rename over the target) and asserts Flux notices.
 *
 * This is the test that fails against the shipped code. The daemon watched only for
 * IN_CLOSE_WRITE, which an atomic rename never delivers on the destination name, so the
 * telemetry cache froze at whatever it happened to read at startup.
 */

namespace {

/// A scratch directory that cleans itself up.
class TempDir {
public:
    TempDir() {
        char pattern[] = "/tmp/flux_inotify_test_XXXXXX";
        const char *made = mkdtemp(pattern);
        if (!made) throw std::runtime_error("mkdtemp failed");
        path_ = made;
    }

    ~TempDir() {
        // Best effort; the test process is short-lived either way.
        const std::string cmd = "rm -rf '" + path_ + "'";
        if (std::system(cmd.c_str()) != 0) { /* ignore */ }
    }

    [[nodiscard]] const std::string &path() const { return path_; }
    [[nodiscard]] std::string file(const std::string &name) const { return path_ + "/" + name; }

private:
    std::string path_;
};

/// Exactly what SynthesisCore's AtomicStatusWriter does.
void atomic_write(const std::string &target, const std::string &content) {
    const std::string tmp = target + ".tmp";

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) throw std::runtime_error("open tmp failed");

    const ssize_t written = ::write(fd, content.data(), content.size());
    if (written != static_cast<ssize_t>(content.size())) {
        ::close(fd);
        throw std::runtime_error("short write");
    }
    ::fsync(fd);
    ::close(fd);

    if (::rename(tmp.c_str(), target.c_str()) != 0) {
        throw std::runtime_error("rename failed");
    }
}

/// The legacy in-place write, for contrast.
void inplace_write(const std::string &target, const std::string &content) {
    std::ofstream out(target, std::ios::trunc);
    out << content;
    out.close();
}

std::string snapshot_text(uint64_t sequence, float headroom = 0.30f) {
    char buffer[1024];
    std::snprintf(
        buffer, sizeof(buffer),
        "schema_version 2\n"
        "sequence %llu\n"
        "updated_elapsed_ms %llu\n"
        "daemon_pid 2841\n"
        "foreground_available 1\n"
        "focused_package com.example.game\n"
        "focused_pid 9821\n"
        "focused_uid 10384\n"
        "screen_available 1\n"
        "screen_awake 1\n"
        "power_available 1\n"
        "battery_saver 0\n"
        "charging_available 1\n"
        "charging_state 1\n"
        "thermal_available 1\n"
        "thermal_valid 1\n"
        "thermal_headroom %.2f\n"
        "thermal_status 2\n"
        "thermal_sample_elapsed_ms 1000\n"
        "thermal_age_ms 10\n"
        "audio_available 1\n"
        "audio_active 0\n"
        "zen_available 1\n"
        "zen_mode 0\n"
        "kernel_is_gki 1\n",
        static_cast<unsigned long long>(sequence),
        static_cast<unsigned long long>(sequence * 500),
        static_cast<double>(headroom)
    );
    return buffer;
}

/// Collects the update events the daemon would act on.
class UpdateCollector {
public:
    /// Mirrors the production handler: react to anything that means "new content".
    void on_event(const struct inotify_event *event, const std::string &path) {
        if (!is_content_update(event->mask)) return;

        TelemetrySnapshot snap;
        const auto result = SynthesisCoreReader::read(snap, path.c_str(), flux_monotonic_ms());

        {
            std::lock_guard<std::mutex> lock(mutex_);
            masks_.push_back(event->mask);
            if (result.ok()) {
                latest_    = snap;
                ++accepted_;
            } else {
                ++rejected_;
            }
        }
        cv_.notify_all();
    }

    /// Wait until at least @p count snapshots have been accepted.
    bool wait_for_accepted(size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return accepted_ >= count; });
    }

    size_t accepted() {
        std::lock_guard<std::mutex> lock(mutex_);
        return accepted_;
    }

    size_t rejected() {
        std::lock_guard<std::mutex> lock(mutex_);
        return rejected_;
    }

    std::optional<TelemetrySnapshot> latest() {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_;
    }

    std::vector<uint32_t> masks() {
        std::lock_guard<std::mutex> lock(mutex_);
        return masks_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    size_t accepted_ = 0;
    size_t rejected_ = 0;
    std::optional<TelemetrySnapshot> latest_;
    std::vector<uint32_t> masks_;
};

constexpr auto TIMEOUT = std::chrono::milliseconds(5000);

} // namespace

// --- The event-mask predicate ---------------------------------------------------

/**
 * The regression, stated as a unit test.
 *
 * An atomic rename delivers IN_MOVED_TO on the destination name. If `is_content_update` does
 * not accept it, Flux never learns that telemetry changed.
 */
TEST("inotify: IN_MOVED_TO counts as a content update") {
    CHECK_MSG(is_content_update(IN_MOVED_TO), "an atomic rename must be seen as new content");
    CHECK(is_content_update(IN_CLOSE_WRITE));
    CHECK(is_content_update(IN_CLOSE_WRITE | IN_MOVED_TO));

    // Things that are not new content.
    CHECK(!is_content_update(IN_OPEN));
    CHECK(!is_content_update(IN_ACCESS));
    CHECK(!is_content_update(IN_MOVED_FROM));
    CHECK(!is_content_update(IN_DELETE));
}

TEST("inotify: removal and overflow are recognised") {
    CHECK(is_removal(IN_DELETE));
    CHECK(is_removal(IN_MOVED_FROM));
    CHECK(!is_removal(IN_MOVED_TO));
    CHECK(is_overflow(IN_Q_OVERFLOW));
}

// --- Real kernel, real watcher ---------------------------------------------------

/**
 * The end-to-end proof.
 *
 * Against the shipped daemon this test fails: zero snapshots are accepted, because the only
 * event the kernel delivers for the watched name is IN_MOVED_TO and the handler ignored it.
 */
TEST("inotify: an atomic rename is observed and parsed") {
    TempDir dir;
    const std::string target = dir.file("synthesis_core.json");

    // Seed the file, as SynthesisCore would have done before Flux started.
    atomic_write(target, snapshot_text(1));

    UpdateCollector collector;
    InotifyWatcher watcher;

    InotifyWatcher::WatchReference ref{
        target,
        [&collector](const struct inotify_event *event, const std::string &path, int, void *) {
            collector.on_event(event, path);
        },
        0,
        nullptr,
    };

    CHECK(watcher.addFile(ref));
    CHECK(watcher.start());

    atomic_write(target, snapshot_text(2, 0.42f));

    CHECK_MSG(collector.wait_for_accepted(1, TIMEOUT), "no update observed for an atomic rename");

    const auto latest = collector.latest();
    CHECK(latest.has_value());
    CHECK_EQ(latest->sequence, 2ULL);
    CHECK_NEAR(latest->thermal_headroom, 0.42f, 1e-5);

    // And confirm it really was a rename event, not an in-place write.
    bool saw_moved_to = false;
    for (const uint32_t mask : collector.masks()) {
        if (mask & IN_MOVED_TO) saw_moved_to = true;
    }
    CHECK_MSG(saw_moved_to, "the kernel should have delivered IN_MOVED_TO");

    watcher.stop();
}

TEST("inotify: an in-place write is still observed") {
    // Not how SynthesisCore writes any more, but the fallback path in the producer does this
    // when rename() fails across filesystems, so it must keep working.
    TempDir dir;
    const std::string target = dir.file("synthesis_core.json");
    inplace_write(target, snapshot_text(1));

    UpdateCollector collector;
    InotifyWatcher watcher;
    watcher.addFile({
        target,
        [&collector](const struct inotify_event *e, const std::string &p, int, void *) {
            collector.on_event(e, p);
        },
        0, nullptr,
    });
    CHECK(watcher.start());

    inplace_write(target, snapshot_text(7, 0.55f));

    CHECK(collector.wait_for_accepted(1, TIMEOUT));
    CHECK_EQ(collector.latest()->sequence, 7ULL);

    watcher.stop();
}

TEST("inotify: 150 rapid atomic replacements all land, ending on the newest") {
    TempDir dir;
    const std::string target = dir.file("synthesis_core.json");
    atomic_write(target, snapshot_text(0));

    UpdateCollector collector;
    InotifyWatcher watcher;
    watcher.addFile({
        target,
        [&collector](const struct inotify_event *e, const std::string &p, int, void *) {
            collector.on_event(e, p);
        },
        0, nullptr,
    });
    CHECK(watcher.start());

    constexpr int updates = 150;
    for (int i = 1; i <= updates; ++i) {
        atomic_write(target, snapshot_text(static_cast<uint64_t>(i)));
    }

    // Events may coalesce, so we do not require exactly `updates` callbacks — only that we
    // saw activity and that we converged on the final state. Converging on the *newest*
    // snapshot is the property that actually matters.
    CHECK(collector.wait_for_accepted(1, TIMEOUT));

    // Give the watcher a moment to drain the queue.
    for (int i = 0; i < 100; ++i) {
        const auto latest = collector.latest();
        if (latest && latest->sequence == updates) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const auto latest = collector.latest();
    CHECK(latest.has_value());
    CHECK_MSG(latest->sequence == updates, "must converge on the newest snapshot");
    CHECK_MSG(collector.rejected() == 0, "no reader should ever observe a partial file");

    watcher.stop();
}

TEST("inotify: a malformed update is rejected and the previous snapshot is retained") {
    TempDir dir;
    const std::string target = dir.file("synthesis_core.json");
    atomic_write(target, snapshot_text(1, 0.33f));

    UpdateCollector collector;
    InotifyWatcher watcher;
    watcher.addFile({
        target,
        [&collector](const struct inotify_event *e, const std::string &p, int, void *) {
            collector.on_event(e, p);
        },
        0, nullptr,
    });
    CHECK(watcher.start());

    atomic_write(target, snapshot_text(2, 0.44f));
    CHECK(collector.wait_for_accepted(1, TIMEOUT));

    // Now a locale-corrupted snapshot, exactly what a comma-decimal producer would emit.
    std::string bad = snapshot_text(3);
    bad.replace(bad.find("thermal_headroom 0.30"), std::string("thermal_headroom 0.30").size(),
                "thermal_headroom 0,30");
    atomic_write(target, bad);

    // Wait for the rejection to be counted.
    for (int i = 0; i < 100 && collector.rejected() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    CHECK_MSG(collector.rejected() >= 1, "the malformed snapshot must be rejected");

    const auto latest = collector.latest();
    CHECK(latest.has_value());
    CHECK_MSG(latest->sequence == 2ULL, "the last good snapshot must survive a bad update");
    CHECK_NEAR(latest->thermal_headroom, 0.44f, 1e-5);

    watcher.stop();
}

/**
 * SynthesisCore crashing, being restarted by the supervisor, and coming back.
 *
 * Flux must keep watching across the gap. Because the watch is on the *directory*, the
 * inode swap performed by rename does not invalidate it.
 */
TEST("inotify: producer disappears and returns, and updates resume") {
    TempDir dir;
    const std::string target = dir.file("synthesis_core.json");
    atomic_write(target, snapshot_text(1));

    UpdateCollector collector;
    InotifyWatcher watcher;
    watcher.addFile({
        target,
        [&collector](const struct inotify_event *e, const std::string &p, int, void *) {
            collector.on_event(e, p);
        },
        0, nullptr,
    });
    CHECK(watcher.start());

    atomic_write(target, snapshot_text(2));
    CHECK(collector.wait_for_accepted(1, TIMEOUT));

    // The producer dies and its status file is removed.
    CHECK_EQ(::unlink(target.c_str()), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // The supervisor restarts it and it publishes again.
    atomic_write(target, snapshot_text(3, 0.61f));

    CHECK_MSG(collector.wait_for_accepted(2, TIMEOUT), "updates must resume after the producer returns");

    const auto latest = collector.latest();
    CHECK(latest.has_value());
    CHECK_EQ(latest->sequence, 3ULL);
    CHECK_NEAR(latest->thermal_headroom, 0.61f, 1e-5);

    watcher.stop();
}

TEST("inotify: the watcher shuts down cleanly while events are in flight") {
    TempDir dir;
    const std::string target = dir.file("synthesis_core.json");
    atomic_write(target, snapshot_text(1));

    UpdateCollector collector;
    {
        InotifyWatcher watcher;
        watcher.addFile({
            target,
            [&collector](const struct inotify_event *e, const std::string &p, int, void *) {
                collector.on_event(e, p);
            },
            0, nullptr,
        });
        CHECK(watcher.start());

        std::thread writer([&] {
            for (int i = 2; i < 60; ++i) {
                atomic_write(target, snapshot_text(static_cast<uint64_t>(i)));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        watcher.stop(); // joins the thread before any fd is closed
        writer.join();

        CHECK(!watcher.isRunning());
    } // destructor must not double-free or fire a callback into freed state

    CHECK(true); // reaching here without a crash or a sanitizer report is the assertion
}
