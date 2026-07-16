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

#include <cstdint>

#include "TelemetryDecoder.hpp"
#include "TelemetrySnapshot.hpp"

namespace flux::telemetry {

/** Thresholds for telemetry health, all measured against Flux's monotonic clock. */
struct FreshnessConfig {
    /// Producer republishes at least every ~2 s; beyond this a snapshot is "delayed".
    int64_t delayed_after_ms = 3000;
    /// Beyond this it is stale: hold current state, never promote.
    int64_t stale_after_ms = 5000;
    /// Beyond this the producer is assumed gone.
    int64_t unavailable_after_ms = 15000;
    /// Consecutive valid samples required after a gap before aggressive promotion is allowed.
    int stabilize_samples = 2;
};

/**
 * @brief Tracks telemetry freshness, sequence continuity and restarts.
 *
 * Fed the outcome of every decode attempt (via observe / observe_failure) and queried for
 * the current health. Monotonic timestamps only — never compared to a wall clock. Sequence
 * regressions are interpreted against the producer PID so that a legitimate daemon restart
 * (new PID, sequence reset to a low value) is not mistaken for corruption.
 */
class TelemetryFreshness {
public:
    explicit TelemetryFreshness(FreshnessConfig config = {}) : config_(config) {}

    /** Record a successfully decoded snapshot. */
    void observe(const RawSnapshot &snapshot, int64_t now_ms);

    /** Record a failed decode (validation failure or unsupported schema). */
    void observe_failure(DecodeError error, int64_t now_ms);

    /** Current health given @p now_ms. */
    [[nodiscard]] TelemetryHealth health(int64_t now_ms) const;

    // Observability, exposed for diagnostics/tests.
    [[nodiscard]] uint64_t last_sequence() const { return last_sequence_; }
    [[nodiscard]] bool last_was_duplicate() const { return last_duplicate_; }
    [[nodiscard]] bool last_was_regression() const { return last_regression_; }
    [[nodiscard]] uint64_t restart_count() const { return restart_count_; }
    [[nodiscard]] bool ever_valid() const { return ever_valid_; }

private:
    FreshnessConfig config_;

    bool ever_valid_ = false;
    bool has_prev_sequence_ = false;
    uint64_t last_sequence_ = 0;
    int last_daemon_pid_ = 0;
    int64_t last_valid_received_ms_ = 0;

    int consecutive_valid_ = 0;
    bool last_duplicate_ = false;
    bool last_regression_ = false;
    uint64_t restart_count_ = 0;

    DecodeError last_error_ = DecodeError::Ok;
    bool last_decode_failed_ = false;
};

} // namespace flux::telemetry
