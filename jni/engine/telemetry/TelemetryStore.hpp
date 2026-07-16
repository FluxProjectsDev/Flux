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

#include <mutex>
#include <optional>

#include "TelemetrySnapshot.hpp"

namespace flux::telemetry {

/** A complete published telemetry state: the snapshot and the health that describes it. */
struct PublishedTelemetry {
    RawSnapshot snapshot;
    TelemetryHealth health = TelemetryHealth::Unavailable;
    uint64_t generation = 0; ///< increments on every publish, for change detection
};

/**
 * @brief Thread-safe holder of the latest complete telemetry state.
 *
 * Publication is whole-value under a mutex: a reader always observes either the entire
 * previous state or the entire new one, never a field-by-field mixture. The mutex is held
 * only for the copy — never across a filesystem or inotify operation. This is a real
 * snapshot, not a plain struct mislabelled "atomic".
 */
class TelemetryStore {
public:
    /** Publish a complete state. Called by the watcher thread. */
    void publish(const RawSnapshot &snapshot, TelemetryHealth health) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = PublishedTelemetry{snapshot, health, ++generation_};
        has_value_ = true;
    }

    /** Update only the health (e.g. as time passes without a new snapshot). */
    void publish_health(TelemetryHealth health) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (has_value_) {
            current_.health = health;
            current_.generation = ++generation_;
        }
    }

    /** A complete copy of the current state, or nullopt if nothing has been published. */
    [[nodiscard]] std::optional<PublishedTelemetry> get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!has_value_) return std::nullopt;
        return current_;
    }

    [[nodiscard]] uint64_t generation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }

private:
    mutable std::mutex mutex_;
    PublishedTelemetry current_;
    bool has_value_ = false;
    uint64_t generation_ = 0;
};

} // namespace flux::telemetry
