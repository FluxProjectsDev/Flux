/*
 * Copyright (C) 2024-2026 Rem01Gaming
 * Copyright (C) 2024-2026 FebriCahyaa
 *
 * Adapted from Encore Tweaks (https://github.com/Rem01Gaming/encore).
 * Modified by the Flux project.
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
#include <deque>
#include <optional>
#include <string>

#include <Flux.hpp>
#include <SynthesisCore.hpp>

/**
 * @file ProfilePolicy.hpp
 * @brief Pure profile-selection policy.
 *
 * Deliberately has no dependency on sysfs, shell, logging or the Android framework: it maps
 * (telemetry, session, config, clock) to (profile, reason). That makes every safety rule in
 * here — thermal direction, hysteresis, priority ordering, the audio guard — executable and
 * assertable on a host machine, which is where they are now tested.
 */

/**
 * @brief Why a profile transition happened.
 *
 * Recorded on every transition. Without this, a user reporting "it dropped out of
 * performance" gives no way to tell a thermal downgrade from a battery-saver switch from a
 * telemetry outage.
 */
enum class TransitionReason {
    None,
    Startup,
    GameStarted,
    GameEnded,
    ThermalPressure,      ///< headroom crossed the enter threshold
    ThermalEmergency,     ///< at or past severe throttling; immediate conservative move
    ThermalRecovered,     ///< headroom fell back below the exit threshold
    BatterySaverEnabled,
    BatterySaverDisabled,
    ScreenOff,
    ScreenOn,
    TelemetryStale,       ///< late; hold, never promote
    TelemetryOffline,     ///< gone; fall back to safe
    TelemetryRestored,
    ChargingStateChanged,
    ConfigurationReloaded,
    UserOverride,
    ShutdownRequested,
};

const char *transition_reason_string(TransitionReason reason);
const char *profile_mode_string(FluxProfileMode mode);

/**
 * @brief Thermal thresholds, in Android headroom units.
 *
 * Headroom is *higher when hotter*: 0.0 is no thermal pressure and 1.0 is the severe
 * throttling threshold. Every comparison below therefore reads "greater than or equal means
 * hotter", which is the exact inverse of what the previous implementation did.
 */
struct ThermalThresholds {
    /// At or above this, drop from Performance to Performance Lite. Chosen below 1.0 so the
    /// downgrade lands *before* the platform itself starts throttling, not after.
    float lite_enter = 0.85f;

    /// At or below this, Lite may recover to Performance. The gap to lite_enter is the
    /// hysteresis band: inside it, whatever tier is current stays current.
    float lite_exit = 0.70f;

    /// At or above this, treat as an emergency regardless of session: the device is past the
    /// severe threshold and a game profile is not defensible.
    float emergency = 1.15f;

    /// Discrete status at or above which we downgrade even if the headroom float disagrees.
    int pressure_status = THERMAL_STATUS_SEVERE;

    /// Discrete status at or above which we treat it as an emergency.
    int emergency_status = THERMAL_STATUS_CRITICAL;

    /// Minimum dwell between thermal-driven transitions, to stop tier flapping.
    int64_t switch_debounce_ms = 5000;

    /// Recovery is additionally delayed: cooling down is not evidence of staying cool.
    int64_t recovery_hold_ms = 15000;

    [[nodiscard]] bool valid() const {
        return lite_exit < lite_enter && lite_enter <= emergency && lite_exit >= 0.0f &&
               switch_debounce_ms >= 0 && recovery_hold_ms >= 0;
    }
};

/** Everything the policy needs to decide. No I/O, no globals. */
struct PolicyInputs {
    TelemetryHealth health = TelemetryHealth::Offline;
    std::optional<TelemetrySnapshot> snapshot;

    bool in_game_session = false;
    std::string active_package;

    /// Game entry requests Lite, or the user enabled enforce_lite_mode globally.
    bool game_forces_lite = false;

    bool shutdown_requested = false;
};

/** Policy state that persists between evaluations. */
struct PolicyState {
    FluxProfileMode current      = PERFCOMMON;
    TransitionReason last_reason = TransitionReason::None;

    int64_t last_thermal_switch_ms = 0;

    // Edge detection, so a *change* in battery saver / screen / charging can be named as the
    // reason rather than inferred.
    bool prev_battery_saver = false;
    bool prev_screen_awake  = true;
    bool prev_charging      = false;
    bool had_telemetry      = false;
};

/** The outcome of one evaluation. */
struct PolicyDecision {
    FluxProfileMode profile = PERFCOMMON;
    TransitionReason reason = TransitionReason::None;
    bool changed            = false;

    /// True when the reason is a safety rule. Safety decisions ignore the audio guard.
    bool safety_driven = false;
};

/** One recorded transition, for diagnostics. */
struct TransitionRecord {
    FluxProfileMode from    = PERFCOMMON;
    FluxProfileMode to      = PERFCOMMON;
    TransitionReason reason = TransitionReason::None;
    int64_t monotonic_ms    = 0;

    std::string package;
    float thermal_headroom = 0.0f;
    bool thermal_valid     = false;
    TelemetryHealth health = TelemetryHealth::Offline;

    bool applied      = true;  ///< false when the apply step reported failure
    std::string apply_error;
};

/**
 * @brief Bounded transition history.
 *
 * Bounded on purpose: this runs for the uptime of the device, and an unbounded log of every
 * profile change is a slow memory leak.
 */
class TransitionHistory {
public:
    explicit TransitionHistory(size_t capacity = 64) : capacity_(capacity) {}

    void record(const TransitionRecord &record) {
        records_.push_back(record);
        while (records_.size() > capacity_) records_.pop_front();
    }

    [[nodiscard]] const std::deque<TransitionRecord> &records() const { return records_; }
    [[nodiscard]] size_t size() const { return records_.size(); }

private:
    size_t capacity_;
    std::deque<TransitionRecord> records_;
};

/**
 * @brief Selects the profile to run.
 *
 * ## Priority order
 *
 * Evaluated top-down; the first rule that fires wins.
 *
 *   1. shutdown requested
 *   2. thermal emergency          — at/past severe throttling
 *   3. telemetry offline          — no producer; fall back to safe
 *   4. battery saver
 *   5. screen off
 *   6. thermal pressure           — inside a game session, downgrade to Lite
 *   7. game session               — Performance (or Lite if configured)
 *   8. default                    — Balance
 *
 * Telemetry that is *stale* (rather than offline) does not select a profile of its own: it
 * acts as a veto on any promotion, so a late snapshot can hold the current profile but can
 * never raise it.
 *
 * ## The audio guard
 *
 * Audio activity suppresses only *cosmetic* transitions — it stops a profile from churning
 * mid-playback for a reason that does not matter. It is checked last and it is ignored
 * entirely when `safety_driven` is set, so it can never defer a thermal downgrade, a battery
 * saver switch or a telemetry fallback. Previously the audio check ran *first* and returned
 * early, which meant a device playing game audio would sit in Performance at any temperature.
 */
class ProfilePolicy {
public:
    explicit ProfilePolicy(ThermalThresholds thresholds = {}) : thresholds_(thresholds) {}

    /** Evaluate the policy. @p state is advanced in place. */
    [[nodiscard]] PolicyDecision evaluate(const PolicyInputs &inputs, PolicyState &state, int64_t now_ms) const;

    [[nodiscard]] const ThermalThresholds &thresholds() const { return thresholds_; }

private:
    ThermalThresholds thresholds_;

    /// Classify thermal pressure from a snapshot. Returns nullopt when thermal cannot be
    /// judged at all (unsupported device, or no valid sample).
    [[nodiscard]] std::optional<TransitionReason> classify_thermal(const TelemetrySnapshot &snap) const;
};
