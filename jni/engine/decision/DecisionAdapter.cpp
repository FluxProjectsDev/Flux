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

#include "DecisionAdapter.hpp"

namespace flux::engine::compat {

FluxProfileMode to_flux_profile(TargetProfile profile) {
    switch (profile) {
        case TargetProfile::Performance: return PERFORMANCE_PROFILE;
        case TargetProfile::PerformanceLite: return PERFORMANCE_LITE_PROFILE;
        case TargetProfile::Balanced: return BALANCE_PROFILE;
        case TargetProfile::PowerSave: return POWERSAVE_PROFILE;
    }
    return BALANCE_PROFILE;
}

TargetProfile from_flux_profile(FluxProfileMode profile) {
    switch (profile) {
        case PERFORMANCE_PROFILE: return TargetProfile::Performance;
        case PERFORMANCE_LITE_PROFILE: return TargetProfile::PerformanceLite;
        case POWERSAVE_PROFILE: return TargetProfile::PowerSave;
        case BALANCE_PROFILE:
        case PERFCOMMON:
            return TargetProfile::Balanced;
    }
    return TargetProfile::Balanced;
}

TransitionReason to_transition_reason(DecisionReason reason) {
    switch (reason) {
        case DecisionReason::startup: return TransitionReason::Startup;
        case DecisionReason::shutdown: return TransitionReason::ShutdownRequested;
        case DecisionReason::thermal_emergency: return TransitionReason::ThermalEmergency;
        case DecisionReason::thermal_pressure: return TransitionReason::ThermalPressure;
        case DecisionReason::thermal_recovered: return TransitionReason::ThermalRecovered;
        case DecisionReason::telemetry_stale: return TransitionReason::TelemetryStale;
        case DecisionReason::telemetry_offline: return TransitionReason::TelemetryOffline;
        case DecisionReason::telemetry_restored: return TransitionReason::TelemetryRestored;
        case DecisionReason::battery_saver_enabled: return TransitionReason::BatterySaverEnabled;
        case DecisionReason::screen_sleeping: return TransitionReason::ScreenOff;
        case DecisionReason::session_started: return TransitionReason::GameStarted;
        case DecisionReason::session_ended: return TransitionReason::GameEnded;
        case DecisionReason::charging_policy: return TransitionReason::ChargingStateChanged;
        case DecisionReason::user_policy: return TransitionReason::UserOverride;
        case DecisionReason::capability_limited: return TransitionReason::UserOverride;
        case DecisionReason::no_transition:
        case DecisionReason::audio_hold:
            return TransitionReason::None;
    }
    return TransitionReason::None;
}

RuntimeSnapshot build_runtime_snapshot(TelemetryHealth health,
                                       const std::optional<TelemetrySnapshot> &snapshot) {
    RuntimeSnapshot rt;
    switch (health) {
        case TelemetryHealth::Healthy: rt.health = DataHealth::Healthy; break;
        case TelemetryHealth::Stale: rt.health = DataHealth::Stale; break;
        case TelemetryHealth::Offline: rt.health = DataHealth::Offline; break;
    }

    if (!snapshot) return rt; // no data: safe defaults (Offline handled by the engine)

    const TelemetrySnapshot &s = *snapshot;

    // Only a validated (available + valid + non-NaN) reading is passed through; the engine
    // never sees a sentinel. has_thermal() already excludes NaN.
    if (s.has_thermal()) {
        rt.thermal = ThermalReading{s.thermal_headroom, s.thermal_status};
    }

    // Unavailable providers degrade to the safe interpretation rather than a false reading.
    rt.screen_awake = s.screen_available ? s.screen_awake : true;
    rt.battery_saver = s.power_available && s.battery_saver;
    rt.charging = s.charging_available && s.charging;
    rt.audio_active = s.audio_available && s.audio_active;
    return rt;
}

CapabilitySnapshot build_capabilities(const std::optional<TelemetrySnapshot> &snapshot) {
    CapabilitySnapshot caps;
    caps.thermal_supported = snapshot && snapshot->thermal_available;
    return caps;
}

} // namespace flux::engine::compat

PolicyDecision FluxDecisionService::decide(const PolicyInputs &inputs, PolicyState &state,
                                           int64_t now_ms) {
    using namespace flux::engine;

    // Respect the daemon's authoritative view of the current profile (e.g. after a
    // failed apply rolled it back), while keeping the richer hysteresis/edge state.
    engine_state_.current = compat::from_flux_profile(state.current);

    DecisionInputs in;
    in.runtime = compat::build_runtime_snapshot(inputs.health, inputs.snapshot);
    in.capabilities = compat::build_capabilities(inputs.snapshot);
    in.session.in_session = inputs.in_game_session;
    in.session.package = inputs.active_package;
    in.session.forces_lite = inputs.game_forces_lite;
    in.shutdown_requested = inputs.shutdown_requested;

    const Decision d = engine_.evaluate(in, engine_state_, now_ms);
    engine_state_ = d.next_state;
    last_ = d;

    // Sync the daemon's legacy view.
    state.current = compat::to_flux_profile(d.desired_profile);
    state.last_reason = compat::to_transition_reason(d.reason);

    PolicyDecision out;
    out.profile = state.current;
    out.reason = state.last_reason;
    out.changed = d.transition_required;
    out.safety_driven = d.safety_driven;
    return out;
}
