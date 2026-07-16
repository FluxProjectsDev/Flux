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

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

/**
 * @file TelemetrySnapshot.hpp
 * @brief Flux V2 telemetry types.
 *
 * This is an independent Flux-owned telemetry layer written from the SynthesisCore
 * telemetry contract v2 and the Flux RuntimeSnapshot semantics. It does not reuse,
 * rename, or translate the legacy jni/base/SynthesisCore reader; it is a parallel,
 * strictly-validating pipeline (decode -> validate -> freshness -> assemble).
 */
namespace flux::telemetry {

/** Supported wire schema range. Versions outside it are rejected, not best-efforted. */
inline constexpr int kSchemaMin = 2;
inline constexpr int kSchemaMax = 2;

/** The `none` sentinel the producer emits when no package is focused. */
inline constexpr const char *kPackageNone = "none";

/** Android discrete thermal status sentinel for "unknown". */
inline constexpr int kThermalStatusUnknown = -1;

/**
 * @brief A strictly-decoded schema-v2 wire snapshot.
 *
 * Every field mirrors a contract key. Availability, validity, and value are kept
 * distinct: `*_available` reflects device capability, `*_valid` reflects whether
 * this cycle produced a usable value, and only then is the value meaningful.
 * A decode never yields a NaN in a field marked valid.
 */
struct RawSnapshot {
    int schema_version = 0;
    uint64_t sequence = 0;
    int64_t updated_elapsed_ms = 0; ///< producer monotonic clock
    int daemon_pid = 0;

    bool foreground_available = false;
    std::string focused_package = kPackageNone;
    int focused_pid = 0;
    int focused_uid = 0;

    bool screen_available = false;
    bool screen_awake = false;

    bool power_available = false;
    bool battery_saver = false;

    bool charging_available = false;
    bool charging = false;

    bool thermal_available = false;
    bool thermal_valid = false;
    float thermal_headroom = std::numeric_limits<float>::quiet_NaN();
    int thermal_status = kThermalStatusUnknown;
    int64_t thermal_sample_elapsed_ms = 0;
    int64_t thermal_age_ms = 0;

    bool audio_available = false;
    bool audio_active = false;

    bool zen_available = false;
    int zen_mode = 0; ///< full enum: 0 off, 1 priority, 2 total silence, 3 alarms only

    bool kernel_is_gki = false;

    /// Flux's own monotonic clock at the moment this snapshot was accepted. Freshness is
    /// judged only against this, never against a wall clock or the producer's clock.
    int64_t received_monotonic_ms = 0;

    [[nodiscard]] bool has_thermal() const {
        return thermal_available && thermal_valid && !std::isnan(thermal_headroom);
    }
};

/** Health of the telemetry stream. Richer than a plain fresh/stale split. */
enum class TelemetryHealth {
    Healthy,          ///< a valid snapshot arrived within the fresh window
    Delayed,          ///< slightly late: hold current state, do not promote
    Stale,            ///< late enough to distrust: conservative, never promote
    Invalid,          ///< last decode failed validation: safe fallback, no reuse
    Unavailable,      ///< no snapshot for a long time / never: producer assumed gone
    UnsupportedSchema ///< producer speaks a schema this build cannot read
};

const char *telemetry_health_name(TelemetryHealth health);

} // namespace flux::telemetry
