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
#include <optional>
#include <string>

/**
 * @file DecisionEngine.hpp
 * @brief Flux V2 Decision Engine — deterministic, side-effect-free policy.
 *
 * This is an independent implementation written from the Flux behavioural
 * specification, the validated RuntimeSnapshot semantics, and the SynthesisCore
 * telemetry v2 contract. It is not a translation of the legacy ProfilePolicy: it
 * has its own vocabulary (TargetProfile / DecisionReason / DecisionPriority), its
 * own input models, and its own explicit priority ladder.
 *
 * The engine maps (RuntimeSnapshot, SessionState, CapabilitySnapshot,
 * DecisionConfig, EngineState, now) to one immutable Decision. It performs no
 * I/O: it does not touch sysfs, run shell, start processes, parse telemetry
 * files, own inotify, read configuration files, or drive the WebUI. Those belong
 * to the Telemetry, Execution, and Supervision layers.
 *
 * evaluate() is *pure*: it does not mutate the EngineState it is given. The next
 * state to persist is returned inside the Decision, so calling evaluate() twice
 * with identical arguments yields an identical Decision — the determinism the
 * tests rely on.
 */
namespace flux::engine {

/** Health of the telemetry feeding a decision. */
enum class DataHealth {
    Healthy, ///< a valid snapshot arrived recently
    Stale,   ///< late: hold, never promote
    Offline, ///< gone: fall back to a safe profile
};

/**
 * @brief The profile the engine wants applied.
 *
 * The enumerators are ordered by performance aggressiveness (PowerSave lowest,
 * Performance highest); the engine relies on that order to reason about
 * promotions vs downgrades. This is Flux-native vocabulary; the mapping to the
 * daemon's FluxProfileMode lives at the integration boundary, not here.
 */
enum class TargetProfile {
    PowerSave = 0,
    Balanced = 1,
    PerformanceLite = 2,
    Performance = 3,
};

const char *target_profile_name(TargetProfile profile);

/** A validated thermal reading. Present only when a usable sample exists. */
struct ThermalReading {
    /// Android headroom semantics: higher is hotter, 1.0 == severe-throttle threshold.
    float headroom = 0.0f;
    /// Android discrete thermal status (THERMAL_STATUS_* value), or a negative unknown.
    int status = -1;
};

/** What the device can actually do. Keeps the engine from planning impossible actions. */
struct CapabilitySnapshot {
    bool thermal_supported = false; ///< getThermalHeadroom() exists on this device
};

/**
 * @brief The single normalized runtime view the engine consumes.
 *
 * "Validated" means invalid values were already rejected upstream: @c thermal is
 * std::nullopt unless a usable, non-NaN sample exists. The engine never sees a
 * NaN headroom, and never parses raw telemetry to obtain one.
 */
struct RuntimeSnapshot {
    DataHealth health = DataHealth::Offline;
    std::optional<ThermalReading> thermal; ///< only present when valid
    bool screen_awake = true;
    bool battery_saver = false;
    bool charging = false;
    bool audio_active = false;
};

/** Foreground/game session context. */
struct SessionState {
    bool in_session = false;
    std::string package;
    bool forces_lite = false; ///< app-specific policy or global enforce-lite requests Lite
};

/** User/device policy configuration. Independent of any file format. */
struct DecisionConfig {
    /// At or above this headroom, a game session downgrades to PerformanceLite.
    float lite_enter = 0.85f;
    /// At or below this headroom, PerformanceLite may recover to Performance.
    float lite_exit = 0.70f;
    /// At or above this headroom, it is a thermal emergency regardless of session.
    float emergency = 1.15f;
    /// Discrete status at/above which pressure is asserted even if the float disagrees.
    int pressure_status = 3; ///< THERMAL_STATUS_SEVERE
    /// Discrete status at/above which an emergency is asserted.
    int emergency_status = 4; ///< THERMAL_STATUS_CRITICAL
    /// Recovery (Lite -> Performance) may not happen until this long after the last
    /// thermal-driven switch. Recovery is deliberately slower than a downgrade.
    int64_t recovery_hold_ms = 15000;
    /// When set, a game session always runs PerformanceLite, never Performance.
    bool enforce_lite_mode = false;

    [[nodiscard]] bool valid() const {
        return lite_exit < lite_enter && lite_enter <= emergency && lite_exit >= 0.0f &&
               recovery_hold_ms >= 0;
    }
};

/** Why the engine reached its decision. Flux-native reason vocabulary. */
enum class DecisionReason {
    startup,
    no_transition,
    shutdown,
    thermal_emergency,
    thermal_pressure,
    thermal_recovered,
    telemetry_stale,
    telemetry_offline,
    telemetry_restored,
    battery_saver_enabled,
    screen_sleeping,
    session_started,
    session_ended,
    charging_policy,
    user_policy,
    capability_limited,
    audio_hold,
};

const char *decision_reason_name(DecisionReason reason);

/**
 * @brief The priority band whose rule won.
 *
 * Rules are evaluated strictly top-down; the first that fires decides. Lower
 * numeric value == higher precedence.
 */
enum class DecisionPriority {
    ShutdownOrFatal = 1,
    ThermalEmergency = 2,
    TelemetrySafety = 3,
    BatterySaver = 4,
    ScreenOff = 5,
    SessionLifecycle = 6,
    ChargingPolicy = 7,
    ConfiguredPerformance = 8,
    AudioStabilityGuard = 9,
    NoncriticalPreference = 10,
};

/** Cross-cutting constraints that shaped or bounded the decision. */
struct SafetyConstraints {
    bool promotion_locked = false;    ///< stale telemetry prevented raising the profile
    bool audio_guard_active = false;  ///< a cosmetic promotion was held during audio playback
    bool thermal_unsupported = false; ///< no thermal capability; thermal rules did not run
    bool restore_settling = false;    ///< just recovered; promotion deferred one cycle
};

/** State persisted between evaluations (hysteresis, edge detection). */
struct EngineState {
    TargetProfile current = TargetProfile::Balanced;
    DecisionReason last_reason = DecisionReason::startup;
    int64_t last_thermal_switch_ms = 0;

    DataHealth prev_health = DataHealth::Offline;
    bool prev_in_session = false;
    bool prev_battery_saver = false;
    bool prev_screen_awake = true;
    bool prev_charging = false;
    bool initialized = false;
};

/** Everything the engine is asked to decide about, in one bundle. */
struct DecisionInputs {
    RuntimeSnapshot runtime;
    SessionState session;
    CapabilitySnapshot capabilities;
    bool shutdown_requested = false;
};

/** The immutable outcome of one evaluation. */
struct Decision {
    TargetProfile desired_profile = TargetProfile::Balanced;
    DecisionReason reason = DecisionReason::startup;
    DecisionPriority priority = DecisionPriority::NoncriticalPreference;

    bool transition_required = false; ///< desired_profile differs from the entry state
    bool recovery_allowed = false;    ///< a thermal recovery was permissible this cycle
    bool safety_driven = false;       ///< a safety rule decided; the audio guard is ignored

    SafetyConstraints constraints;
    DataHealth health = DataHealth::Offline;
    std::string explanation; ///< short human-readable cause, for diagnostics

    EngineState next_state; ///< the state the caller must persist
};

/**
 * @brief The deterministic Flux V2 policy.
 *
 * ## Priority ladder (first match wins)
 *
 *   1. shutdown / fatal
 *   2. thermal emergency        (safety; ignores audio; acts on last valid reading)
 *   3. telemetry safety         (offline -> safe; stale -> promotion veto)
 *   4. battery saver
 *   5. screen off
 *   6. session / game lifecycle (with thermal-pressure hysteresis inside it)
 *   7. charging policy
 *   8. configured performance policy (idle default)
 *   9. audio stability guard    (suppresses only cosmetic promotions)
 *  10. noncritical preference
 */
class DecisionEngine {
public:
    explicit DecisionEngine(DecisionConfig config = {}) : config_(config) {}

    /** Evaluate the policy. Pure: @p state is not modified; persist Decision::next_state. */
    [[nodiscard]] Decision evaluate(const DecisionInputs &in, const EngineState &state,
                                    int64_t now_ms) const;

    [[nodiscard]] const DecisionConfig &config() const { return config_; }

private:
    DecisionConfig config_;
};

} // namespace flux::engine
