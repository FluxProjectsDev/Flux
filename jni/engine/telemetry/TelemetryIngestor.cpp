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

#include "TelemetryIngestor.hpp"

namespace flux::telemetry {

void TelemetryIngestor::republish_health(int64_t now_ms) {
    // Keep the last good snapshot in place; only the health label changes as it ages or after
    // a failed decode. If there is no good snapshot yet, there is nothing to republish and the
    // store correctly reports "no value" (the reader treats that as unavailable).
    if (last_good_) store_.publish(*last_good_, freshness_.health(now_ms));
}

void TelemetryIngestor::ingest(std::optional<std::string> content, bool uncertain, int64_t now_ms) {
    last_uncertain_ = uncertain;

    if (!content) {
        // Target absent/unreadable: no new snapshot. Do not clobber the last good one; let it
        // age through the freshness model on the next tick.
        republish_health(now_ms);
        return;
    }

    const DecodeResult decoded = decoder_.decode(*content, now_ms);
    if (!decoded.ok()) {
        // Malformed replacement: record the failure, but the previous complete snapshot
        // survives — republished under whatever health the failure produces (Invalid, or the
        // aged health of the retained snapshot). Never a partial mixture.
        freshness_.observe_failure(decoded.error, now_ms);
        republish_health(now_ms);
        return;
    }

    freshness_.observe(decoded.snapshot, now_ms);
    last_good_ = decoded.snapshot;
    store_.publish(decoded.snapshot, freshness_.health(now_ms));
}

void TelemetryIngestor::tick(int64_t now_ms) {
    if (last_good_) {
        store_.publish_health(freshness_.health(now_ms));
    }
}

} // namespace flux::telemetry
