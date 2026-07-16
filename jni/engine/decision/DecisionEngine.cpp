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

#include "DecisionEngine.hpp"

namespace flux::engine {

const char *target_profile_name(TargetProfile profile) {
    switch (profile) {
        case TargetProfile::PowerSave: return "power_save";
        case TargetProfile::Balanced: return "balanced";
        case TargetProfile::PerformanceLite: return "performance_lite";
        case TargetProfile::Performance: return "performance";
    }
    return "unknown";
}

const char *decision_reason_name(DecisionReason reason) {
    switch (reason) {
        case DecisionReason::startup: return "startup";
        case DecisionReason::no_transition: return "no_transition";
        case DecisionReason::shutdown: return "shutdown";
        case DecisionReason::thermal_emergency: return "thermal_emergency";
        case DecisionReason::thermal_pressure: return "thermal_pressure";
        case DecisionReason::thermal_recovered: return "thermal_recovered";
        case DecisionReason::telemetry_stale: return "telemetry_stale";
        case DecisionReason::telemetry_offline: return "telemetry_offline";
        case DecisionReason::telemetry_restored: return "telemetry_restored";
        case DecisionReason::battery_saver_enabled: return "battery_saver_enabled";
        case DecisionReason::screen_sleeping: return "screen_sleeping";
        case DecisionReason::session_started: return "session_started";
        case DecisionReason::session_ended: return "session_ended";
        case DecisionReason::charging_policy: return "charging_policy";
        case DecisionReason::user_policy: return "user_policy";
        case DecisionReason::capability_limited: return "capability_limited";
        case DecisionReason::audio_hold: return "audio_hold";
    }
    return "unknown";
}

namespace {

/// Aggressiveness rank; higher means more performance. Matches enum order.
int rank(TargetProfile p) { return static_cast<int>(p); }

/// How the current thermal reading classifies, once capability is accounted for.
enum class ThermClass {
    Unsupported, ///< device has no thermal API; treat as cool but emit no thermal reasons
    NoData,      ///< supported, but no valid sample this cycle; cannot confirm cool
    Cool,        ///< headroom at or below the recovery threshold
    Band,        ///< inside the hysteresis band; hold whatever tier is current
    Pressure,    ///< at/above the downgrade threshold (or severe status)
    Emergency,   ///< at/above the emergency threshold (or critical status)
};

ThermClass classify(const RuntimeSnapshot &rt, const CapabilitySnapshot &caps,
                    const DecisionConfig &cfg) {
    if (!caps.thermal_supported) return ThermClass::Unsupported;
    if (!rt.thermal) return ThermClass::NoData;

    const float h = rt.thermal->headroom;
    const int s = rt.thermal->status;

    if (h >= cfg.emergency || s >= cfg.emergency_status) return ThermClass::Emergency;
    if (h >= cfg.lite_enter || s >= cfg.pressure_status) return ThermClass::Pressure;
    if (h <= cfg.lite_exit) return ThermClass::Cool;
    return ThermClass::Band;
}

} // namespace

Decision DecisionEngine::evaluate(const DecisionInputs &in, const EngineState &state,
                                  int64_t now_ms) const {
    const RuntimeSnapshot &rt = in.runtime;
    const ThermClass tc = classify(rt, in.capabilities, config_);

    EngineState ns = state;
    ns.initialized = true;

    // Candidate produced by the priority ladder.
    TargetProfile cand = state.current;
    DecisionReason reason = DecisionReason::no_transition;
    DecisionPriority prio = DecisionPriority::NoncriticalPreference;
    bool safety = false;
    bool recovery_allowed = false;

    // --- Priority ladder (first match wins) --------------------------------
    if (in.shutdown_requested) {
        cand = TargetProfile::Balanced;
        reason = DecisionReason::shutdown;
        prio = DecisionPriority::ShutdownOrFatal;
        safety = true;
    } else if (tc == ThermClass::Emergency) {
        // Acts on the last valid reading regardless of staleness: a hot device is
        // hot whether or not a fresh sample arrived this instant.
        cand = TargetProfile::Balanced;
        reason = DecisionReason::thermal_emergency;
        prio = DecisionPriority::ThermalEmergency;
        safety = true;
        ns.last_thermal_switch_ms = now_ms;
    } else if (rt.health == DataHealth::Offline) {
        cand = TargetProfile::Balanced;
        reason = DecisionReason::telemetry_offline;
        prio = DecisionPriority::TelemetrySafety;
        safety = true;
    } else if (rt.battery_saver) {
        cand = TargetProfile::PowerSave;
        reason = DecisionReason::battery_saver_enabled;
        prio = DecisionPriority::BatterySaver;
        safety = true;
    } else if (!rt.screen_awake) {
        cand = TargetProfile::Balanced;
        reason = DecisionReason::screen_sleeping;
        prio = DecisionPriority::ScreenOff;
        safety = true;
    } else if (in.session.in_session) {
        prio = DecisionPriority::SessionLifecycle;
        const TargetProfile cur = state.current;
        const bool force_lite = in.session.forces_lite || config_.enforce_lite_mode;
        const bool fresh_session = !state.prev_in_session;

        if (force_lite) {
            cand = TargetProfile::PerformanceLite;
            reason = fresh_session ? DecisionReason::session_started
                                   : DecisionReason::capability_limited;
        } else {
            switch (tc) {
                case ThermClass::Pressure:
                    cand = TargetProfile::PerformanceLite;
                    if (cur != TargetProfile::PerformanceLite) {
                        reason = DecisionReason::thermal_pressure;
                        safety = true; // a downgrade under heat is a safety move
                        ns.last_thermal_switch_ms = now_ms;
                    } else {
                        reason = DecisionReason::no_transition;
                    }
                    break;

                case ThermClass::Cool:
                    if (cur == TargetProfile::PerformanceLite) {
                        // Recovery is gated: cooling is not immediate licence to promote.
                        recovery_allowed =
                            (now_ms - state.last_thermal_switch_ms) >= config_.recovery_hold_ms;
                        if (recovery_allowed) {
                            cand = TargetProfile::Performance;
                            reason = DecisionReason::thermal_recovered;
                            ns.last_thermal_switch_ms = now_ms;
                        } else {
                            cand = TargetProfile::PerformanceLite;
                            reason = DecisionReason::no_transition;
                        }
                    } else {
                        cand = TargetProfile::Performance;
                        reason = fresh_session ? DecisionReason::session_started
                                               : DecisionReason::no_transition;
                    }
                    break;

                case ThermClass::Band:
                case ThermClass::NoData:
                    // Hold the current performance tier; never promote out of Lite without
                    // positive evidence of cooling.
                    if (cur == TargetProfile::PerformanceLite) {
                        cand = TargetProfile::PerformanceLite;
                        reason = DecisionReason::no_transition;
                    } else {
                        cand = TargetProfile::Performance;
                        reason = fresh_session ? DecisionReason::session_started
                                               : DecisionReason::no_transition;
                    }
                    break;

                case ThermClass::Unsupported:
                    cand = TargetProfile::Performance;
                    reason = fresh_session ? DecisionReason::session_started
                                           : DecisionReason::no_transition;
                    break;

                case ThermClass::Emergency:
                    // Unreachable: emergency is handled above the session branch.
                    cand = TargetProfile::Balanced;
                    reason = DecisionReason::thermal_emergency;
                    break;
            }
        }
    } else {
        // Idle: screen on, not in a session, no battery saver, no emergency/offline.
        prio = DecisionPriority::ConfiguredPerformance;
        cand = TargetProfile::Balanced;
        if (state.prev_in_session) {
            reason = DecisionReason::session_ended;
        } else if (state.prev_charging != rt.charging) {
            reason = DecisionReason::charging_policy;
            prio = DecisionPriority::ChargingPolicy;
        } else {
            reason = (state.current == TargetProfile::Balanced) ? DecisionReason::no_transition
                                                                : DecisionReason::user_policy;
        }
    }

    SafetyConstraints constraints;
    constraints.thermal_unsupported = (tc == ThermClass::Unsupported);

    // --- Promotion guards (stale / just-restored) --------------------------
    // A promotion is any move to a more aggressive profile than the current one.
    const bool is_promotion = rank(cand) > rank(state.current);
    const bool just_restored =
        state.prev_health != DataHealth::Healthy && rt.health == DataHealth::Healthy;

    if (is_promotion && rt.health == DataHealth::Stale) {
        cand = state.current;
        reason = DecisionReason::telemetry_stale;
        prio = DecisionPriority::TelemetrySafety;
        safety = true;
        constraints.promotion_locked = true;
    } else if (is_promotion && just_restored) {
        // Do not leap to an aggressive profile the instant telemetry returns; require one
        // settled healthy cycle first.
        cand = state.current;
        reason = DecisionReason::telemetry_restored;
        prio = DecisionPriority::TelemetrySafety;
        safety = true;
        constraints.restore_settling = true;
    }

    // --- Audio stability guard (cosmetic promotions only) ------------------
    // Never applies to safety decisions, so it can never defer a thermal downgrade, a
    // battery-saver switch, an emergency, or a telemetry fallback. It only holds back a
    // thermal *recovery* promotion so the profile does not churn upward mid-playback.
    if (!safety && rt.audio_active && reason == DecisionReason::thermal_recovered &&
        rank(cand) > rank(state.current)) {
        cand = state.current;
        reason = DecisionReason::audio_hold;
        prio = DecisionPriority::AudioStabilityGuard;
        constraints.audio_guard_active = true;
        recovery_allowed = false;
    }

    // --- Assemble the immutable decision -----------------------------------
    Decision d;
    d.desired_profile = cand;
    d.reason = reason;
    d.priority = prio;
    d.transition_required = (cand != state.current);
    d.recovery_allowed = recovery_allowed;
    d.safety_driven = safety;
    d.constraints = constraints;
    d.health = rt.health;
    d.explanation = std::string(decision_reason_name(reason)) + " -> " + target_profile_name(cand);

    ns.current = cand;
    ns.last_reason = reason;
    ns.prev_health = rt.health;
    ns.prev_in_session = in.session.in_session;
    ns.prev_battery_saver = rt.battery_saver;
    ns.prev_screen_awake = rt.screen_awake;
    ns.prev_charging = rt.charging;
    d.next_state = ns;

    return d;
}

} // namespace flux::engine
