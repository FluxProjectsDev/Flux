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

#pragma once

#include <cstdio>
#include <mutex>
#include <string>

#include <Flux.hpp>

struct SynthesisCore {
    std::string focused_app;
    pid_t focused_pid = 0;
    uid_t focused_uid = 0;
    bool screen_awake = false;
    bool battery_saver = false;
    int zen_mode = 0;
    bool charging = false;
    float thermal_headroom = -1.0f; // -1.0 = unsupported (API < 31)
    bool audio_active = false;
    bool thermal_api_available = false; ///< true when getThermalHeadroom() resolved on API 31+
    bool kernel_is_gki = false;         ///< true when kernel reports GKI (-androidXX- in uname -r)
};

namespace SynthesisCoreReader {

/**
 * @brief Parse SYNTHESIS_CORE_FILE into @p out.
 *
 * @param out   Destination struct.
 * @param path  Path to the status file (defaults to SYNTHESIS_CORE_FILE).
 * @return true  if the file was opened and at least one field was parsed.
 * @return false if the file could not be opened.
 */
bool read(SynthesisCore &out, const char *path = SYNTHESIS_CORE_FILE);

} // namespace SynthesisCoreReader

/**
 * @brief Thread-safe wrapper around the latest SynthesisCore snapshot.
 *
 * The InotifyHandler writes a fresh snapshot on every IN_CLOSE_WRITE
 * event; the main daemon loop reads the snapshot without blocking.
 */
class SynthesisCoreCache {
public:
    /** Update the cached snapshot (called from inotify thread). */
    void update(const SynthesisCore &s) {
        std::lock_guard<std::mutex> lk(mtx_);
        status_ = s;
        valid_ = true;
    }

    /**
     * @brief Copy the latest snapshot into @p out
     * @return false if no snapshot has been stored yet.
     */
    bool get(SynthesisCore &out) const {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!valid_) return false;
        out = status_;
        return true;
    }

    bool is_valid() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return valid_;
    }

private:
    mutable std::mutex mtx_;
    SynthesisCore status_;
    bool valid_ = false;
};

/** Global cache shared between the inotify thread and the main daemon. */
extern SynthesisCoreCache synthesis_core_cache;
