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

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace flux::telemetry {

/**
 * @brief Watches SynthesisCore's status file for atomic replacement.
 *
 * Independent Flux implementation written from documented Linux inotify semantics
 * (`man 7 inotify`) and the SynthesisCore write contract (write temp, fsync, rename
 * temp -> target). It watches the *directory*, because a rename swaps the target's
 * inode: a watch on the file itself, or a wait for IN_CLOSE_WRITE on the target,
 * would miss every atomic update. The completed-update signal is IN_MOVED_TO with
 * the target's name.
 *
 * Ownership and shutdown are deterministic: one owned worker thread, woken for
 * teardown through an eventfd, joined before any descriptor is closed. The callback
 * is never invoked after stop()/destruction, and no descriptor is used after close.
 *
 * The watcher only delivers file *content*; decoding, validation and publication are
 * the consumer's job (see TelemetryIngestor), keeping this class free of the schema.
 */
class AtomicStatusWatcher {
public:
    /**
     * @param content The current file bytes, or std::nullopt when the target is absent
     *                (deleted / not yet created / unreadable).
     * @param uncertain True after an inotify queue overflow: events may have been lost,
     *                  so the consumer should treat this as a full resync.
     */
    using Callback = std::function<void(std::optional<std::string> content, bool uncertain)>;

    AtomicStatusWatcher(std::string directory, std::string filename, Callback callback);
    ~AtomicStatusWatcher();

    AtomicStatusWatcher(const AtomicStatusWatcher &) = delete;
    AtomicStatusWatcher &operator=(const AtomicStatusWatcher &) = delete;

    /**
     * @brief Establish the directory watch, reconcile the current file, then start watching.
     *
     * Ordering matters: the watch is added *before* the initial read, so any replacement
     * that races the startup read is already queued and delivered once the worker runs.
     * @return false on setup failure (see last_error()).
     */
    bool start();

    /** Stop the worker and release resources. Idempotent; also called by the destructor. */
    void stop();

    /** Force an immediate reread + callback (e.g. an external resync request). */
    void resync(bool uncertain = false);

    [[nodiscard]] std::string last_error() const { return last_error_; }

    /// Max status-file size read into memory, matching the decoder's input bound.
    static constexpr size_t kMaxFileBytes = 64 * 1024;

private:
    std::string directory_;
    std::string filename_;
    std::string target_path_;
    Callback callback_;

    int inotify_fd_ = -1;
    int watch_fd_ = -1;
    int wake_fd_ = -1; // eventfd for deterministic wake-up
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::string last_error_;

    bool add_watch();
    void run();
    void deliver(bool uncertain);
    std::optional<std::string> read_target() const;
};

} // namespace flux::telemetry
