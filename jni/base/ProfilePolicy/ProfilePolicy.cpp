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

#include "ProfilePolicy.hpp"

const char *transition_reason_string(TransitionReason reason) {
    switch (reason) {
        case TransitionReason::None: return "none";
        case TransitionReason::Startup: return "startup";
        case TransitionReason::GameStarted: return "game_started";
        case TransitionReason::GameEnded: return "game_ended";
        case TransitionReason::ThermalPressure: return "thermal_pressure";
        case TransitionReason::ThermalEmergency: return "thermal_emergency";
        case TransitionReason::ThermalRecovered: return "thermal_recovered";
        case TransitionReason::BatterySaverEnabled: return "battery_saver_enabled";
        case TransitionReason::BatterySaverDisabled: return "battery_saver_disabled";
        case TransitionReason::ScreenOff: return "screen_off";
        case TransitionReason::ScreenOn: return "screen_on";
        case TransitionReason::TelemetryStale: return "telemetry_stale";
        case TransitionReason::TelemetryOffline: return "telemetry_offline";
        case TransitionReason::TelemetryRestored: return "telemetry_restored";
        case TransitionReason::ChargingStateChanged: return "charging_state_changed";
        case TransitionReason::ConfigurationReloaded: return "configuration_reloaded";
        case TransitionReason::UserOverride: return "user_override";
        case TransitionReason::ShutdownRequested: return "shutdown_requested";
    }
    return "unknown";
}

const char *profile_mode_string(FluxProfileMode mode) {
    switch (mode) {
        case PERFCOMMON: return "perfcommon";
        case PERFORMANCE_PROFILE: return "performance";
        case PERFORMANCE_LITE_PROFILE: return "performance_lite";
        case BALANCE_PROFILE: return "balance";
        case POWERSAVE_PROFILE: return "powersave";
    }
    return "unknown";
}

namespace {

/// Ranking used only to decide what counts as a "promotion" for the stale-telemetry veto.
/// Higher means more aggressive.
int aggressiveness(FluxProfileMode mode) {
    switch (mode) {
        case POWERSAVE_PROFILE: return 0;
        case PERFCOMMON: return 1;
        case BALANCE_PROFILE: return 2;
        case PERFORMANCE_LITE_PROFILE: return 3;
        case PERFORMANCE_PROFILE: return 4;
    }
    return 0;
}

bool is_perf_tier(FluxProfileMode mode) {
    return mode == PERFORMANCE_PROFILE || mode == PERFORMANCE_LITE_PROFILE;
}

} // namespace

std::optional<TransitionReason> ProfilePolicy::classify_thermal(const TelemetrySnapshot &snap) const {
    // Unsupported hardware: thermal is simply not a factor in the decision. This is not the
    // same as "cool" — it means we have no opinion, and the policy must not invent one.
    if (!snap.thermal_available) return std::nullopt;

    // Supported but this sample is not usable. We cannot judge, so we say so; the caller
    // holds the current tier rather than promoting on a guess.
    if (!snap.has_thermal()) return std::nullopt;

    const float headroom = snap.thermal_headroom;
    const int status     = snap.thermal_status;

    // Emergency: past the severe threshold by a margin, or the platform itself says critical.
    // Either signal alone is sufficient — a vendor whose headroom float is unreliable may
    // still report an honest status, and vice versa.
    if (headroom >= thresholds_.emergency ||
        (status != THERMAL_STATUS_UNKNOWN && status >= thresholds_.emergency_status)) {
        return TransitionReason::ThermalEmergency;
    }

    if (headroom >= thresholds_.lite_enter ||
        (status != THERMAL_STATUS_UNKNOWN && status >= thresholds_.pressure_status)) {
        return TransitionReason::ThermalPressure;
    }

    if (headroom <= thresholds_.lite_exit) {
        return TransitionReason::ThermalRecovered;
    }

    // Inside the hysteresis band: neither hot enough to drop nor cool enough to recover.
    return TransitionReason::None;
}

PolicyDecision ProfilePolicy::evaluate(const PolicyInputs &inputs, PolicyState &state, int64_t now_ms) const {
    PolicyDecision decision;
    decision.profile = state.current;

    auto settle = [&](FluxProfileMode profile, TransitionReason reason, bool safety) {
        decision.profile       = profile;
        decision.reason        = reason;
        decision.safety_driven = safety;
    };

    // --- 1. Shutdown ---------------------------------------------------------
    if (inputs.shutdown_requested) {
        settle(BALANCE_PROFILE, TransitionReason::ShutdownRequested, true);
        decision.changed = (decision.profile != state.current);
        state.current    = decision.profile;
        state.last_reason = decision.reason;
        return decision;
    }

    const auto &snap = inputs.snapshot;

    // --- 2/3. Telemetry offline ----------------------------------------------
    // No producer at all. We cannot see the device, so we must not drive it hard. Note this
    // does *not* stop the daemon: Flux keeps running and recovers when telemetry returns.
    if (inputs.health == TelemetryHealth::Offline || !snap.has_value()) {
        settle(BALANCE_PROFILE, TransitionReason::TelemetryOffline, true);
        state.had_telemetry = false;
        decision.changed    = (decision.profile != state.current);
        state.current       = decision.profile;
        state.last_reason   = decision.reason;
        return decision;
    }

    const TelemetrySnapshot &s = *snap;
    const bool restored        = !state.had_telemetry;
    state.had_telemetry        = true;

    // --- 2. Thermal emergency ------------------------------------------------
    // Ranked above battery saver, screen state and the game session on purpose: nothing the
    // user has configured is a reason to keep pushing a device that is already throttling.
    const auto thermal = classify_thermal(s);
    if (thermal == TransitionReason::ThermalEmergency) {
        settle(BALANCE_PROFILE, TransitionReason::ThermalEmergency, true);
        // Emergencies are not debounced. Debouncing exists to stop cosmetic flapping, and a
        // device at critical temperature is not a cosmetic concern.
        state.last_thermal_switch_ms = now_ms;
        decision.changed             = (decision.profile != state.current);
        state.current                = decision.profile;
        state.last_reason            = decision.reason;
        return decision;
    }

    // --- 4. Battery saver -----------------------------------------------------
    if (s.power_available && s.battery_saver) {
        const auto reason = state.prev_battery_saver ? TransitionReason::BatterySaverEnabled
                                                     : TransitionReason::BatterySaverEnabled;
        settle(POWERSAVE_PROFILE, reason, true);
        state.prev_battery_saver = true;
        decision.changed         = (decision.profile != state.current);
        state.current            = decision.profile;
        state.last_reason        = decision.reason;
        return decision;
    }
    const bool battery_saver_cleared = state.prev_battery_saver && s.power_available && !s.battery_saver;
    state.prev_battery_saver         = s.power_available && s.battery_saver;

    // --- 5. Screen off --------------------------------------------------------
    // Nothing is being rendered; a game session in the background does not justify a
    // performance profile.
    if (s.screen_available && !s.screen_awake) {
        settle(BALANCE_PROFILE, TransitionReason::ScreenOff, true);
        state.prev_screen_awake = false;
        decision.changed        = (decision.profile != state.current);
        state.current           = decision.profile;
        state.last_reason       = decision.reason;
        return decision;
    }
    const bool screen_turned_on = !state.prev_screen_awake && s.screen_awake;
    state.prev_screen_awake     = !s.screen_available || s.screen_awake;

    // --- 6/7. Game session ----------------------------------------------------
    if (inputs.in_game_session && !inputs.active_package.empty()) {
        FluxProfileMode target = PERFORMANCE_PROFILE;
        TransitionReason reason =
            restored ? TransitionReason::TelemetryRestored
                     : (state.current == PERFORMANCE_PROFILE || state.current == PERFORMANCE_LITE_PROFILE)
                           ? TransitionReason::None
                           : TransitionReason::GameStarted;
        bool safety = false;

        if (inputs.game_forces_lite) {
            // Explicit user/config choice. Not a safety rule, but not overridable by thermal
            // *recovery* either — it simply pins the tier.
            target = PERFORMANCE_LITE_PROFILE;
        } else if (thermal == TransitionReason::ThermalPressure) {
            target = PERFORMANCE_LITE_PROFILE;
            reason = TransitionReason::ThermalPressure;
            safety = true;
        } else if (!thermal.has_value()) {
            // Thermal cannot be judged. If we are already in Lite, stay there — promoting on
            // absent data is exactly the "stale telemetry promotes performance" failure.
            // If thermal is *unsupported* on this device we have never been in a
            // thermally-driven Lite, so Performance is correct and reachable.
            target = s.thermal_available && state.current == PERFORMANCE_LITE_PROFILE
                         ? PERFORMANCE_LITE_PROFILE
                         : PERFORMANCE_PROFILE;
        } else if (state.current == PERFORMANCE_LITE_PROFILE) {
            // Recovery path, and the only place hysteresis applies.
            const bool cool_enough    = (thermal == TransitionReason::ThermalRecovered);
            const int64_t since_switch = now_ms - state.last_thermal_switch_ms;
            const bool held_long_enough = since_switch >= thresholds_.recovery_hold_ms;

            if (cool_enough && held_long_enough) {
                target = PERFORMANCE_PROFILE;
                reason = TransitionReason::ThermalRecovered;
            } else {
                // Inside the hysteresis band, or not cooled for long enough. Hold.
                target = PERFORMANCE_LITE_PROFILE;
            }
        }

        // Debounce thermal-driven tier changes so headroom hovering on a threshold does not
        // thrash CPU/GPU limits. Emergencies bypass this (handled above).
        const bool is_thermal_switch =
            (reason == TransitionReason::ThermalPressure || reason == TransitionReason::ThermalRecovered);
        if (is_thermal_switch && target != state.current) {
            const int64_t since = now_ms - state.last_thermal_switch_ms;
            if (since < thresholds_.switch_debounce_ms) {
                target = state.current; // hold
                reason = TransitionReason::None;
                safety = false;
            } else {
                state.last_thermal_switch_ms = now_ms;
            }
        }

        settle(target, reason, safety);
    } else if (battery_saver_cleared) {
        settle(BALANCE_PROFILE, TransitionReason::BatterySaverDisabled, false);
    } else if (screen_turned_on) {
        settle(BALANCE_PROFILE, TransitionReason::ScreenOn, false);
    } else if (restored) {
        settle(BALANCE_PROFILE, TransitionReason::TelemetryRestored, false);
    } else if (s.charging_available && s.charging != state.prev_charging) {
        settle(BALANCE_PROFILE, TransitionReason::ChargingStateChanged, false);
    } else {
        // --- 8. Default --------------------------------------------------------
        const auto reason = (state.current == BALANCE_PROFILE) ? TransitionReason::None
                                                               : TransitionReason::GameEnded;
        settle(BALANCE_PROFILE, reason, false);
    }
    state.prev_charging = s.charging_available && s.charging;

    // --- Stale telemetry veto --------------------------------------------------
    // Late telemetry may hold the current profile but must never raise it. A snapshot we are
    // no longer confident in is not evidence that it is safe to push the device harder.
    if (inputs.health == TelemetryHealth::Stale &&
        aggressiveness(decision.profile) > aggressiveness(state.current)) {
        decision.profile       = state.current;
        decision.reason        = TransitionReason::TelemetryStale;
        decision.safety_driven = true;
    }

    // --- 9. Audio stability guard ----------------------------------------------
    // Last, and never over a safety decision. Suppresses only cosmetic churn during playback.
    if (!decision.safety_driven && s.audio_available && s.audio_active && is_perf_tier(state.current) &&
        decision.profile != state.current) {
        decision.profile = state.current;
        decision.reason  = TransitionReason::None;
    }

    decision.changed  = (decision.profile != state.current);
    state.current     = decision.profile;
    if (decision.reason != TransitionReason::None) state.last_reason = decision.reason;
    return decision;
}
