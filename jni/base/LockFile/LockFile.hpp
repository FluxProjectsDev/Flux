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
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

/**
 * @brief Single-holder file lock built on Linux Open File Description (OFD) locks.
 *
 * This is an independent Flux implementation designed from the documented Linux
 * `fcntl(F_OFD_SETLK)` semantics (`man 2 fcntl`, "Open file description locks")
 * and Flux's daemon requirements. It is not derived from any prior lock helper.
 *
 * ## Why OFD locks, not classic POSIX (`F_SETLK`) locks
 *
 * Classic POSIX record locks are owned by the *(process, inode)* pair, which has
 * two well-known hazards for a long-lived daemon:
 *
 *   1. Closing *any* file descriptor referring to the locked file drops *every*
 *      lock the process holds on that inode — so an unrelated `is_locked()` probe
 *      that opens and closes the same path silently releases the real lock.
 *   2. Locks are not associated with a specific open file description, so two
 *      parts of the same process cannot hold independent locks on one file.
 *
 * OFD locks (Linux 3.15+, bionic API 24+) are owned by the *open file
 * description* instead. A probe opened on its own descriptor never disturbs the
 * lock held on another descriptor, and closing the owning descriptor — including
 * implicitly, when the process dies — is what releases the lock. That is exactly
 * the ownership model a single-instance guard needs.
 *
 * ## Threading
 *
 * A single instance is not safe to call concurrently from arbitrary threads. The
 * only internally-threaded feature is watch()/unwatch(), whose lifecycle is
 * managed here with deterministic join-on-teardown; the watcher touches only the
 * members documented below.
 */
class LockFile {
public:
    enum class AcquireMode { Blocking, NonBlocking };
    enum class LockType { Exclusive, Shared };

    /**
     * @brief Invoked once by a watch() thread when the watched lock becomes free.
     *
     * Unlike the interface this replaces, the callback fires *only* on the free
     * transition. Tearing the watcher down with unwatch() (or destruction) does
     * not invoke it — a deliberate teardown is not a "became free" event, and the
     * sole consumer only ever acted on the free case anyway.
     */
    using WatchCallback = std::function<void()>;

    /** Default watch() poll cadence. Detecting a producer exit within this bound
     *  is ample; a shorter value only adds wakeups. */
    static constexpr std::chrono::milliseconds kDefaultPollInterval{250};

    explicit LockFile(std::string path);
    ~LockFile();

    LockFile(const LockFile &) = delete;
    LockFile &operator=(const LockFile &) = delete;

    // Moving stops any active watcher on both objects first, so a moved-from
    // instance never leaves a thread referring to relocated state.
    LockFile(LockFile &&o) noexcept;
    LockFile &operator=(LockFile &&o) noexcept;

    /**
     * @brief Acquire the lock on this open file description.
     * @return true if the lock is now held by this instance.
     *
     * NonBlocking returns false immediately when another holder exists.
     * Blocking waits, transparently retrying across EINTR.
     */
    bool acquire(AcquireMode mode = AcquireMode::Blocking, LockType type = LockType::Exclusive);

    /**
     * @brief Release a lock held by this instance.
     * @return true if a held lock was released; false if none was held.
     */
    bool release();

    /**
     * @brief True if the file is locked by this instance or by any other holder.
     */
    bool is_locked() const;

    /**
     * @brief Watch the file and invoke @p callback once when it becomes free.
     *
     * Any previous watcher is stopped first. If the file is already free the
     * callback fires promptly on the next poll.
     */
    void watch(WatchCallback callback, std::chrono::milliseconds poll_interval = kDefaultPollInterval);

    /** @brief Stop an active watcher and join its thread. Idempotent. */
    void unwatch();

    const std::string &path() const noexcept { return path_; }

    /** @brief True when this instance currently holds the lock. */
    bool holds_lock() const noexcept { return fd_ != -1 && locked_; }

    /** @brief Human-readable description of the last failing syscall, for logs. */
    std::string last_error() const;

private:
    std::string path_;
    int fd_ = -1;
    bool locked_ = false;
    int last_errno_ = 0;

    // Watcher state. watch_cv_ is notified by unwatch() to end the poll wait promptly.
    std::thread watcher_;
    std::mutex watch_mutex_;
    std::condition_variable watch_cv_;
    std::atomic<bool> stop_watch_{false};

    int open_fd() noexcept;
    void close_fd() noexcept;
    bool other_holder_exists() const;

    void stop_watcher() noexcept;
};
