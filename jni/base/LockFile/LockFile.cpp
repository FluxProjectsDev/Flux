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

// OFD lock constants (F_OFD_SETLK et al.) are gated behind _GNU_SOURCE in glibc.
// bionic exposes them unconditionally, so this only matters for the host build.
#ifndef _GNU_SOURCE
    #define _GNU_SOURCE
#endif

#include "LockFile.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

// Canonical Linux values, identical across the architectures Flux targets. Defined
// defensively in case a toolchain's <fcntl.h> predates OFD locks; the kernel
// (3.15+) and bionic (API 24+) both understand them regardless of header vintage.
#ifndef F_OFD_GETLK
    #define F_OFD_GETLK 36
    #define F_OFD_SETLK 37
    #define F_OFD_SETLKW 38
#endif

namespace {

/**
 * Build a whole-file flock request. l_pid MUST be zero for OFD commands; a
 * non-zero l_pid makes the kernel reject the request with EINVAL.
 */
struct flock make_flock(short l_type) noexcept {
    struct flock fl{};
    fl.l_type = l_type;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0; // 0 == "to end of file", i.e. the whole file
    fl.l_pid = 0;
    return fl;
}

short fcntl_type_for(LockFile::LockType type) noexcept {
    return (type == LockFile::LockType::Exclusive) ? F_WRLCK : F_RDLCK;
}

/** Open an independent descriptor on @p path for probing, or -1 with errno set. */
int open_probe(const std::string &path) noexcept {
    return ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
}

/**
 * F_OFD_GETLK against @p probe_fd: does a *conflicting* lock exist that this open
 * file description does not itself hold? Retries across EINTR.
 */
bool conflicting_lock_present(int probe_fd) noexcept {
    struct flock fl = make_flock(F_WRLCK);
    int ret;
    do {
        ret = ::fcntl(probe_fd, F_OFD_GETLK, &fl);
    } while (ret != 0 && errno == EINTR);
    if (ret != 0) return false; // cannot determine; report "not locked"
    return fl.l_type != F_UNLCK;
}

} // namespace

LockFile::LockFile(std::string path) : path_(std::move(path)) {}

LockFile::~LockFile() {
    unwatch();
    release();
    close_fd();
}

LockFile::LockFile(LockFile &&o) noexcept {
    // Join the source's watcher before stealing its state, so no live thread is
    // left referring to relocated members.
    o.stop_watcher();
    path_ = std::move(o.path_);
    fd_ = std::exchange(o.fd_, -1);
    locked_ = std::exchange(o.locked_, false);
    last_errno_ = std::exchange(o.last_errno_, 0);
}

LockFile &LockFile::operator=(LockFile &&o) noexcept {
    if (this != &o) {
        stop_watcher();
        release();
        close_fd();
        o.stop_watcher();

        path_ = std::move(o.path_);
        fd_ = std::exchange(o.fd_, -1);
        locked_ = std::exchange(o.locked_, false);
        last_errno_ = std::exchange(o.last_errno_, 0);
    }
    return *this;
}

int LockFile::open_fd() noexcept {
    if (fd_ != -1) return fd_;
    fd_ = ::open(path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd_ == -1) last_errno_ = errno;
    return fd_;
}

void LockFile::close_fd() noexcept {
    if (fd_ != -1) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool LockFile::acquire(AcquireMode mode, LockType type) {
    if (open_fd() == -1) return false;

    struct flock fl = make_flock(fcntl_type_for(type));
    const int cmd = (mode == AcquireMode::Blocking) ? F_OFD_SETLKW : F_OFD_SETLK;

    while (::fcntl(fd_, cmd, &fl) != 0) {
        if (errno == EINTR) continue; // interrupted; retry the same request
        last_errno_ = errno;          // EACCES/EAGAIN == held elsewhere
        return false;
    }

    locked_ = true;
    last_errno_ = 0;
    return true;
}

bool LockFile::release() {
    if (!locked_ || fd_ == -1) return false;

    struct flock fl = make_flock(F_UNLCK);
    while (::fcntl(fd_, F_OFD_SETLK, &fl) != 0) {
        if (errno == EINTR) continue;
        last_errno_ = errno;
        return false;
    }

    locked_ = false;
    return true;
}

bool LockFile::other_holder_exists() const {
    int probe = open_probe(path_);
    if (probe == -1) return false;
    const bool present = conflicting_lock_present(probe);
    ::close(probe);
    return present;
}

bool LockFile::is_locked() const {
    if (locked_) return true;
    return other_holder_exists();
}

void LockFile::watch(WatchCallback callback, std::chrono::milliseconds poll_interval) {
    unwatch();
    stop_watch_.store(false, std::memory_order_release);

    watcher_ = std::thread([this, cb = std::move(callback), poll_interval] {
        const int probe = open_probe(path_);
        if (probe == -1) return; // cannot watch; no spurious callback

        bool became_free = false;
        {
            std::unique_lock<std::mutex> lock(watch_mutex_);
            while (!stop_watch_.load(std::memory_order_acquire)) {
                if (!conflicting_lock_present(probe)) {
                    became_free = true;
                    break;
                }
                // Sleep until the next poll, or wake immediately on unwatch().
                watch_cv_.wait_for(lock, poll_interval, [this] {
                    return stop_watch_.load(std::memory_order_acquire);
                });
            }
        }

        ::close(probe);
        if (became_free) cb();
    });
}

void LockFile::unwatch() {
    stop_watcher();
}

void LockFile::stop_watcher() noexcept {
    if (!watcher_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(watch_mutex_);
        stop_watch_.store(true, std::memory_order_release);
    }
    watch_cv_.notify_all();
    watcher_.join();
    stop_watch_.store(false, std::memory_order_release);
}

std::string LockFile::last_error() const {
    if (last_errno_ == 0) return "no error";
    return std::strerror(last_errno_);
}
