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

#include "TelemetryFreshness.hpp"

namespace flux::telemetry {

const char *telemetry_health_name(TelemetryHealth health) {
    switch (health) {
        case TelemetryHealth::Healthy: return "healthy";
        case TelemetryHealth::Delayed: return "delayed";
        case TelemetryHealth::Stale: return "stale";
        case TelemetryHealth::Invalid: return "invalid";
        case TelemetryHealth::Unavailable: return "unavailable";
        case TelemetryHealth::UnsupportedSchema: return "unsupported_schema";
    }
    return "unknown";
}

void TelemetryFreshness::observe(const RawSnapshot &snapshot, int64_t now_ms) {
    last_decode_failed_ = false;
    last_error_ = DecodeError::Ok;
    last_duplicate_ = false;
    last_regression_ = false;

    const bool restart = has_prev_sequence_ && snapshot.daemon_pid != last_daemon_pid_;
    if (restart) {
        // A new producer PID with a fresh (often reset) sequence is a legitimate restart,
        // not corruption. Rebaseline instead of flagging a regression, and require the
        // stabilization window again before promoting.
        ++restart_count_;
        consecutive_valid_ = 0;
    }

    if (has_prev_sequence_ && !restart) {
        if (snapshot.sequence == last_sequence_) {
            last_duplicate_ = true; // same content republished (heartbeat); not new evidence
        } else if (snapshot.sequence < last_sequence_) {
            last_regression_ = true; // same PID but sequence went backwards: suspicious
            consecutive_valid_ = 0;
        }
    }

    // A gap in wall-time freshness also restarts the stabilization count.
    if (ever_valid_ && (now_ms - last_valid_received_ms_) > config_.stale_after_ms) {
        consecutive_valid_ = 0;
    }

    if (!last_duplicate_ && !last_regression_) {
        ++consecutive_valid_;
    }

    ever_valid_ = true;
    has_prev_sequence_ = true;
    last_sequence_ = snapshot.sequence;
    last_daemon_pid_ = snapshot.daemon_pid;
    last_valid_received_ms_ = now_ms;
}

void TelemetryFreshness::observe_failure(DecodeError error, int64_t now_ms) {
    (void)now_ms;
    last_decode_failed_ = true;
    last_error_ = error;
    last_duplicate_ = false;
    last_regression_ = false;
    consecutive_valid_ = 0; // a bad decode must not count toward restore stabilization
}

TelemetryHealth TelemetryFreshness::health(int64_t now_ms) const {
    if (last_decode_failed_) {
        if (last_error_ == DecodeError::UnsupportedSchema) return TelemetryHealth::UnsupportedSchema;
        // A validation failure with no prior good snapshot is Invalid; with a prior good
        // one, fall through to age-based health (the last good snapshot still ages out).
        if (!ever_valid_) return TelemetryHealth::Invalid;
    }

    if (!ever_valid_) return TelemetryHealth::Unavailable;

    const int64_t age = now_ms - last_valid_received_ms_;
    if (age > config_.unavailable_after_ms) return TelemetryHealth::Unavailable;
    if (age > config_.stale_after_ms) return TelemetryHealth::Stale;
    if (age > config_.delayed_after_ms) return TelemetryHealth::Delayed;

    // Fresh, but hold back from "Healthy" (which the engine may promote on) until the
    // producer has proven stable after a gap/restart. This prevents restored/stale flapping.
    if (consecutive_valid_ < config_.stabilize_samples) return TelemetryHealth::Delayed;
    return TelemetryHealth::Healthy;
}

} // namespace flux::telemetry
