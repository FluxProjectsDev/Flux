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

#include <optional>
#include <string>

#include "TelemetryDecoder.hpp"
#include "TelemetryFreshness.hpp"
#include "TelemetryStore.hpp"

namespace flux::telemetry {

/**
 * @brief Turns raw status-file content into published telemetry.
 *
 * This is the glue between AtomicStatusWatcher and the store: content -> decode ->
 * freshness -> publish. It is where the "a malformed replacement must not destroy the
 * last complete valid snapshot" guarantee lives — a failed decode republishes the last
 * good snapshot under a degraded health rather than clobbering it, and never leaves a
 * partially-parsed mixture in the store.
 *
 * It is not thread-safe by itself; call ingest()/tick() from a single owner (the watcher
 * callback runs on the watcher's worker thread, so route both through that thread or add a
 * mutex if you call tick() elsewhere). The store it publishes to is thread-safe for readers.
 */
class TelemetryIngestor {
public:
    explicit TelemetryIngestor(TelemetryStore &store, FreshnessConfig freshness_config = {})
        : store_(store), freshness_(freshness_config) {}

    /**
     * @brief Ingest one delivery from the watcher.
     * @param content nullopt when the target is absent (deleted / unreadable).
     * @param uncertain true after a queue overflow (a lost-events resync).
     */
    void ingest(std::optional<std::string> content, bool uncertain, int64_t now_ms);

    /** Advance published health as time passes without a new snapshot. Call each eval tick. */
    void tick(int64_t now_ms);

    [[nodiscard]] const TelemetryFreshness &freshness() const { return freshness_; }
    [[nodiscard]] bool last_uncertain() const { return last_uncertain_; }

private:
    TelemetryStore &store_;
    TelemetryDecoder decoder_;
    TelemetryFreshness freshness_;
    std::optional<RawSnapshot> last_good_;
    bool last_uncertain_ = false;

    void republish_health(int64_t now_ms);
};

} // namespace flux::telemetry
