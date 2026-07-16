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

#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>

#include <sys/types.h>

/**
 * @file SynthesisCore.hpp
 * @brief Consumer side of the SynthesisCore telemetry contract (schema v2).
 *
 * Deliberately free of Android, logging and syscall dependencies so the parser and the
 * freshness model can be compiled and tested on a host toolchain. See jni/tests/.
 */

/** Schema version this build of Flux understands. */
inline constexpr int SYNTHESIS_SCHEMA_VERSION = 2;

/** Android PowerManager thermal status values, as emitted by SynthesisCore. */
enum ThermalStatus : int {
    THERMAL_STATUS_UNKNOWN   = -1,
    THERMAL_STATUS_NONE      = 0,
    THERMAL_STATUS_LIGHT     = 1,
    THERMAL_STATUS_MODERATE  = 2,
    THERMAL_STATUS_SEVERE    = 3,
    THERMAL_STATUS_CRITICAL  = 4,
    THERMAL_STATUS_EMERGENCY = 5,
    THERMAL_STATUS_SHUTDOWN  = 6,
};

/**
 * @brief A complete telemetry snapshot from SynthesisCore.
 *
 * Availability, validity and value are three separate things and are modelled as such.
 * @c thermal_headroom is meaningful only when @c thermal_valid is true; otherwise it is
 * NaN — never 0.0 and never -1.0, because both of those are numerically comparable against
 * a threshold, and the previous contract's -1.0 sentinel was in fact compared that way.
 *
 * Thermal headroom follows Android's real semantics: **0.0 means no thermal pressure, and
 * 1.0 means the device has reached the severe-throttling threshold.** Values above 1.0 are
 * legal and mean the device is past that threshold. Higher is hotter.
 */
struct TelemetrySnapshot {
    int schema_version         = 0;
    uint64_t sequence          = 0;
    int64_t updated_elapsed_ms = 0; ///< Producer's monotonic clock (SystemClock.elapsedRealtime)
    pid_t daemon_pid           = 0;

    bool foreground_available = false;
    std::string focused_package;    ///< "none" when nothing is focused
    pid_t focused_pid = 0;
    uid_t focused_uid = 0;

    bool screen_available = false;
    bool screen_awake     = false;

    bool power_available = false;
    bool battery_saver   = false;

    bool charging_available = false;
    bool charging           = false;

    bool thermal_available            = false; ///< getThermalHeadroom() exists on this device
    bool thermal_valid                = false; ///< this sample produced a usable number
    float thermal_headroom            = std::numeric_limits<float>::quiet_NaN();
    int thermal_status                = THERMAL_STATUS_UNKNOWN;
    int64_t thermal_sample_elapsed_ms = 0;
    int64_t thermal_age_ms            = 0;

    bool audio_available = false;
    bool audio_active    = false;

    bool zen_available = false;
    int zen_mode       = 0; ///< Full enum: 0 off, 1 priority, 2 total silence, 3 alarms only

    bool kernel_is_gki = false;

    /**
     * @brief Flux's own monotonic clock reading when this snapshot was accepted.
     *
     * Freshness must be judged against a clock Flux controls. The producer's
     * @c updated_elapsed_ms proves the producer is advancing, but a dead producer's
     * timestamp simply stops changing — it never reports "I am dead". Only local receipt
     * time distinguishes "unchanged because nothing happened" from "unchanged because
     * nobody is writing it".
     */
    int64_t received_monotonic_ms = 0;

    /** True when a usable thermal headroom reading is present. */
    [[nodiscard]] bool has_thermal() const {
        return thermal_available && thermal_valid && !std::isnan(thermal_headroom);
    }
};

/** Why a parse was rejected. Surfaced to diagnostics rather than swallowed. */
enum class ParseError {
    None,
    FileUnreadable,
    MissingSchemaVersion,
    UnsupportedSchemaVersion,
    MalformedNumber,       ///< includes locale-corrupted floats such as "0,84"
    ImplausibleIdentifier, ///< negative or absurd pid/uid
    MissingRequiredField,
};

const char *parse_error_string(ParseError err);

namespace SynthesisCoreReader {

struct ParseResult {
    ParseError error = ParseError::None;
    std::string detail;

    [[nodiscard]] bool ok() const { return error == ParseError::None; }
};

/**
 * @brief Parse a schema-v2 snapshot from @p text.
 *
 * Parsing is all-or-nothing. The snapshot is assembled in a local temporary and copied into
 * @p out only once every required field has been seen and validated. A malformed update
 * therefore cannot leave @p out holding a mixture of new and stale fields — which the
 * previous field-by-field parser could, and did.
 *
 * Unknown keys are ignored, so a future producer may add fields without breaking this build.
 * A schema version other than #SYNTHESIS_SCHEMA_VERSION is rejected outright rather than
 * read on a best-effort basis.
 *
 * @param out Written only on success.
 * @param text Full file contents.
 * @param received_monotonic_ms Flux's monotonic clock at the time of receipt.
 */
ParseResult parse(TelemetrySnapshot &out, const std::string &text, int64_t received_monotonic_ms);

/** Read and parse the status file at @p path. */
ParseResult read(TelemetrySnapshot &out, const char *path, int64_t received_monotonic_ms);

} // namespace SynthesisCoreReader

/**
 * @brief How healthy the telemetry stream currently is.
 *
 * Telemetry that is merely late must not be treated the same as telemetry that has stopped.
 */
enum class TelemetryHealth {
    Healthy, ///< a valid snapshot arrived recently
    Stale,   ///< nothing new for a while: hold the current profile, never promote
    Offline, ///< nothing for a long time, or never: fall back to a safe profile
};

const char *telemetry_health_string(TelemetryHealth health);

/** Freshness thresholds, measured against Flux's own monotonic clock. */
struct FreshnessPolicy {
    /// No accepted snapshot within this window: stop trusting it enough to raise performance.
    /// SynthesisCore republishes at least every 2 s even when nothing changes, so 5 s is
    /// two missed heartbeats — late enough to be real, early enough to matter.
    int64_t stale_after_ms = 5000;

    /// No accepted snapshot within this window: assume the producer is gone.
    int64_t offline_after_ms = 15000;
};

/**
 * @brief Thread-safe holder for the newest accepted snapshot.
 *
 * Publication is whole-snapshot under a mutex. The previous implementation described a plain
 * struct copy as an "atomic snapshot"; it was neither atomic, nor — once the parser began
 * writing directly into it — a snapshot.
 */
class SynthesisCoreCache {
public:
    /** Publish a fully-parsed snapshot. Called from the inotify thread. */
    void update(const TelemetrySnapshot &s) {
        std::lock_guard<std::mutex> lk(mtx_);
        status_ = s;
        valid_  = true;
        ++accepted_count_;
    }

    /** @return the newest snapshot, or nullopt if none has ever been accepted. */
    [[nodiscard]] std::optional<TelemetrySnapshot> get() const {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!valid_) return std::nullopt;
        return status_;
    }

    /** Record a failed parse for diagnostics. Never disturbs the last good snapshot. */
    void record_parse_error(ParseError err, const std::string &detail) {
        std::lock_guard<std::mutex> lk(mtx_);
        last_error_        = err;
        last_error_detail_ = detail;
        ++parse_error_count_;
    }

    [[nodiscard]] ParseError last_error() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return last_error_;
    }

    [[nodiscard]] std::string last_error_detail() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return last_error_detail_;
    }

    [[nodiscard]] uint64_t parse_error_count() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return parse_error_count_;
    }

    [[nodiscard]] uint64_t accepted_count() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return accepted_count_;
    }

    /**
     * @brief Classify freshness against @p now_ms on Flux's monotonic clock.
     *
     * Deliberately independent of the producer's timestamp, for the reason documented on
     * TelemetrySnapshot::received_monotonic_ms.
     */
    [[nodiscard]] TelemetryHealth health(int64_t now_ms, const FreshnessPolicy &policy) const {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!valid_) return TelemetryHealth::Offline;

        const int64_t age = now_ms - status_.received_monotonic_ms;
        if (age >= policy.offline_after_ms) return TelemetryHealth::Offline;
        if (age >= policy.stale_after_ms) return TelemetryHealth::Stale;
        return TelemetryHealth::Healthy;
    }

    /** Age in ms of the newest accepted snapshot, or -1 when there is none. */
    [[nodiscard]] int64_t age_ms(int64_t now_ms) const {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!valid_) return -1;
        return now_ms - status_.received_monotonic_ms;
    }

private:
    mutable std::mutex mtx_;
    TelemetrySnapshot status_;
    bool valid_ = false;

    ParseError last_error_ = ParseError::None;
    std::string last_error_detail_;
    uint64_t parse_error_count_ = 0;
    uint64_t accepted_count_    = 0;
};

/** Global cache shared between the inotify thread and the main daemon. */
extern SynthesisCoreCache synthesis_core_cache;

/** Monotonic milliseconds. The clock every Flux freshness decision is made against. */
int64_t flux_monotonic_ms();
