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

// Host tests for the independent OFD-lock-based LockFile.
//
// These exercise the real kernel: OFD locks are a kernel facility, so acquisition,
// cross-open-description contention, release, close-on-exit behaviour, move
// semantics, and the watch()/unwatch() lifecycle are all tested for real rather
// than mocked.

#include "TestFramework.hpp"

#include "LockFile.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <unistd.h>

namespace {

using namespace std::chrono_literals;

/** A unique lock path that is removed when the test scope ends. */
struct TempLock {
    std::filesystem::path path;

    TempLock() {
        static std::atomic<unsigned> counter{0};
        const auto n = counter.fetch_add(1);
        path = std::filesystem::temp_directory_path() /
               ("flux-lockfile-test-" + std::to_string(::getpid()) + "-" + std::to_string(n));
        std::filesystem::remove(path);
    }
    ~TempLock() { std::filesystem::remove(path); }

    std::string str() const { return path.string(); }
};

/** Poll @p pred up to @p budget, so timing tests are robust to scheduler jitter. */
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds budget = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

} // namespace

TEST("lock: a fresh path is not locked") {
    TempLock t;
    LockFile lock(t.str());
    CHECK(!lock.is_locked());
    CHECK(!lock.holds_lock());
}

TEST("lock: acquire takes the lock and reports it held") {
    TempLock t;
    LockFile lock(t.str());
    CHECK(lock.acquire(LockFile::AcquireMode::NonBlocking, LockFile::LockType::Exclusive));
    CHECK(lock.holds_lock());
    CHECK(lock.is_locked());
    CHECK_EQ(lock.path(), t.str());
}

TEST("lock: a second open description cannot take a held lock (contention)") {
    TempLock t;
    LockFile a(t.str());
    LockFile b(t.str());

    CHECK(a.acquire(LockFile::AcquireMode::NonBlocking));
    // OFD locks conflict across open file descriptions, even within one process.
    CHECK(!b.acquire(LockFile::AcquireMode::NonBlocking));
    CHECK(!b.holds_lock());
    // b can still observe that the file is locked by someone.
    CHECK(b.is_locked());
}

TEST("lock: release frees the lock for another holder") {
    TempLock t;
    LockFile a(t.str());
    LockFile b(t.str());

    CHECK(a.acquire(LockFile::AcquireMode::NonBlocking));
    CHECK(a.release());
    CHECK(!a.holds_lock());

    // Now b can take it.
    CHECK(b.acquire(LockFile::AcquireMode::NonBlocking));
    CHECK(b.holds_lock());
}

TEST("lock: releasing when nothing is held returns false") {
    TempLock t;
    LockFile lock(t.str());
    CHECK(!lock.release());
}

TEST("lock: destroying the holder releases the lock (RAII / process exit analog)") {
    TempLock t;
    {
        LockFile holder(t.str());
        CHECK(holder.acquire(LockFile::AcquireMode::NonBlocking));
        CHECK(holder.is_locked());
    } // holder destroyed: OFD lock released on descriptor close, exactly as on exit

    LockFile next(t.str());
    CHECK(!next.is_locked());
    CHECK(next.acquire(LockFile::AcquireMode::NonBlocking));
}

TEST("lock: is_locked probe does not disturb a lock held on another descriptor") {
    TempLock t;
    LockFile holder(t.str());
    CHECK(holder.acquire(LockFile::AcquireMode::NonBlocking));

    // Repeated probing (which opens and closes its own descriptor) must not drop
    // the holder's lock — the classic-POSIX-lock hazard OFD locks avoid.
    for (int i = 0; i < 5; ++i) {
        LockFile probe(t.str());
        CHECK(probe.is_locked());
        CHECK(!probe.acquire(LockFile::AcquireMode::NonBlocking));
    }
    CHECK(holder.holds_lock());
    CHECK(holder.is_locked());
}

TEST("lock: move construction transfers ownership and empties the source") {
    TempLock t;
    LockFile a(t.str());
    CHECK(a.acquire(LockFile::AcquireMode::NonBlocking));

    LockFile b(std::move(a));
    CHECK(b.holds_lock());
    CHECK_EQ(b.path(), t.str());

    // A third holder still cannot take it: ownership moved, it was not released.
    LockFile c(t.str());
    CHECK(!c.acquire(LockFile::AcquireMode::NonBlocking));

    CHECK(b.release());
    CHECK(c.acquire(LockFile::AcquireMode::NonBlocking));
}

TEST("lock: move assignment releases the destination's prior lock") {
    TempLock t1;
    TempLock t2;
    LockFile a(t1.str());
    LockFile b(t2.str());
    CHECK(a.acquire(LockFile::AcquireMode::NonBlocking));
    CHECK(b.acquire(LockFile::AcquireMode::NonBlocking));

    b = std::move(a); // b's old lock on t2 must be freed
    CHECK_EQ(b.path(), t1.str());
    CHECK(b.holds_lock());

    LockFile reclaim(t2.str());
    CHECK(reclaim.acquire(LockFile::AcquireMode::NonBlocking));
}

TEST("watch: fires once when the watched lock becomes free") {
    TempLock t;
    auto holder = std::make_unique<LockFile>(t.str());
    CHECK(holder->acquire(LockFile::AcquireMode::NonBlocking));

    LockFile watcher(t.str());
    std::atomic<int> fired{0};
    watcher.watch([&fired] { fired.fetch_add(1); }, 10ms);

    // Still held: no callback yet.
    std::this_thread::sleep_for(60ms);
    CHECK_EQ(fired.load(), 0);

    holder.reset(); // release by destroying the holder
    CHECK(wait_until([&fired] { return fired.load() == 1; }));

    watcher.unwatch();
    CHECK_EQ(fired.load(), 1); // exactly once, no spurious extra fire
}

TEST("watch: unwatch stops the watcher without firing the callback") {
    TempLock t;
    LockFile holder(t.str());
    CHECK(holder.acquire(LockFile::AcquireMode::NonBlocking));

    LockFile watcher(t.str());
    std::atomic<int> fired{0};
    watcher.watch([&fired] { fired.fetch_add(1); }, 10ms);

    std::this_thread::sleep_for(40ms);
    watcher.unwatch(); // deliberate teardown while the lock is still held

    // Even after the holder later releases, the torn-down watcher must not fire.
    CHECK(holder.release());
    std::this_thread::sleep_for(60ms);
    CHECK_EQ(fired.load(), 0);
}

TEST("watch: destroying a watching LockFile joins its thread cleanly") {
    TempLock t;
    LockFile holder(t.str());
    CHECK(holder.acquire(LockFile::AcquireMode::NonBlocking));

    std::atomic<int> fired{0};
    {
        LockFile watcher(t.str());
        watcher.watch([&fired] { fired.fetch_add(1); }, 10ms);
        std::this_thread::sleep_for(30ms);
    } // destructor must unwatch()+join deterministically, no hang, no fire
    CHECK_EQ(fired.load(), 0);
}
