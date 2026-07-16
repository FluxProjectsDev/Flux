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

#include "AtomicStatusWatcher.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace flux::telemetry {

namespace {
constexpr uint32_t kWatchMask = IN_MOVED_TO | IN_CREATE | IN_CLOSE_WRITE | IN_DELETE |
                                IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF;
} // namespace

AtomicStatusWatcher::AtomicStatusWatcher(std::string directory, std::string filename,
                                         Callback callback)
    : directory_(std::move(directory)), filename_(std::move(filename)),
      callback_(std::move(callback)) {
    target_path_ = directory_ + "/" + filename_;
}

AtomicStatusWatcher::~AtomicStatusWatcher() { stop(); }

std::optional<std::string> AtomicStatusWatcher::read_target() const {
    const int fd = ::open(target_path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1) return std::nullopt;

    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > kMaxFileBytes) break; // decoder will reject oversized input
    }
    ::close(fd);
    if (n == -1 && errno == EINTR) {
        // A partial read interrupted by a signal; let the caller treat it as unreadable.
        return std::nullopt;
    }
    return out;
}

void AtomicStatusWatcher::deliver(bool uncertain) { callback_(read_target(), uncertain); }

bool AtomicStatusWatcher::add_watch() {
    watch_fd_ = ::inotify_add_watch(inotify_fd_, directory_.c_str(), kWatchMask);
    if (watch_fd_ == -1) {
        last_error_ = std::string("inotify_add_watch(") + directory_ + "): " + std::strerror(errno);
        return false;
    }
    return true;
}

bool AtomicStatusWatcher::start() {
    if (running_.load()) return true;

    inotify_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_ == -1) {
        last_error_ = std::string("inotify_init1: ") + std::strerror(errno);
        return false;
    }
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ == -1) {
        last_error_ = std::string("eventfd: ") + std::strerror(errno);
        ::close(inotify_fd_);
        inotify_fd_ = -1;
        return false;
    }

    // Watch BEFORE the initial read: a replacement that races startup is then already queued
    // and delivered once the worker runs, closing the read/registration race window.
    if (!add_watch()) {
        ::close(wake_fd_);
        ::close(inotify_fd_);
        wake_fd_ = inotify_fd_ = -1;
        return false;
    }

    running_.store(true);
    deliver(/*uncertain=*/false); // initial reconciliation
    worker_ = std::thread(&AtomicStatusWatcher::run, this);
    return true;
}

void AtomicStatusWatcher::resync(bool uncertain) {
    if (running_.load()) deliver(uncertain);
}

void AtomicStatusWatcher::run() {
    std::vector<char> buffer(64 * 1024);

    while (running_.load()) {
        struct pollfd fds[2];
        fds[0] = {inotify_fd_, POLLIN, 0};
        fds[1] = {wake_fd_, POLLIN, 0};

        const int ready = ::poll(fds, 2, -1);
        if (ready == -1) {
            if (errno == EINTR) continue;
            last_error_ = std::string("poll: ") + std::strerror(errno);
            break;
        }

        if (fds[1].revents & POLLIN) {
            uint64_t drained = 0;
            const ssize_t got = ::read(wake_fd_, &drained, sizeof(drained));
            (void)got;
            break; // stop requested
        }
        if (!(fds[0].revents & POLLIN)) continue;

        const ssize_t len = ::read(inotify_fd_, buffer.data(), buffer.size());
        if (len <= 0) {
            if (len == -1 && errno == EINTR) continue;
            continue;
        }

        // Coalesce a batch: at most one reread per read(), landing on the newest content.
        bool needs_reread = false;
        bool uncertain = false;
        bool target_gone = false;
        bool rewatch = false;

        for (char *ptr = buffer.data(); ptr < buffer.data() + len;) {
            auto *ev = reinterpret_cast<struct inotify_event *>(ptr);
            const std::string name = (ev->len > 0) ? std::string(ev->name) : std::string();

            if (ev->mask & IN_Q_OVERFLOW) {
                // Events were lost: do a full resync and flag the consumer.
                needs_reread = true;
                uncertain = true;
            } else if (ev->mask & IN_IGNORED) {
                rewatch = true; // watch auto-removed (dir replaced); re-establish it
            } else if ((ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF))) {
                rewatch = true;
            } else if (name == filename_) {
                if (ev->mask & (IN_MOVED_TO | IN_CREATE | IN_CLOSE_WRITE)) {
                    needs_reread = true; // IN_MOVED_TO is the atomic-replacement signal
                } else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
                    target_gone = true;
                }
            }
            ptr += sizeof(struct inotify_event) + ev->len;
        }

        if (rewatch) {
            // The directory watch was invalidated; rebuild it and resync.
            if (watch_fd_ != -1) ::inotify_rm_watch(inotify_fd_, watch_fd_);
            watch_fd_ = -1;
            if (add_watch()) {
                needs_reread = true;
                uncertain = true;
            }
        }

        if (needs_reread) {
            deliver(uncertain); // a fresh read always wins over an earlier delete in the batch
        } else if (target_gone) {
            callback_(std::nullopt, false);
        }
    }
}

void AtomicStatusWatcher::stop() {
    if (!running_.exchange(false)) {
        // Never started, or already stopped: still clean up any partially-opened fds.
        if (wake_fd_ != -1) {
            ::close(wake_fd_);
            wake_fd_ = -1;
        }
        if (inotify_fd_ != -1) {
            if (watch_fd_ != -1) ::inotify_rm_watch(inotify_fd_, watch_fd_);
            ::close(inotify_fd_);
            inotify_fd_ = watch_fd_ = -1;
        }
        return;
    }

    // Wake the worker, then join before closing any descriptor it uses.
    if (wake_fd_ != -1) {
        const uint64_t one = 1;
        const ssize_t put = ::write(wake_fd_, &one, sizeof(one));
        (void)put;
    }
    if (worker_.joinable()) worker_.join();

    if (watch_fd_ != -1) ::inotify_rm_watch(inotify_fd_, watch_fd_);
    if (wake_fd_ != -1) ::close(wake_fd_);
    if (inotify_fd_ != -1) ::close(inotify_fd_);
    watch_fd_ = wake_fd_ = inotify_fd_ = -1;
}

} // namespace flux::telemetry
