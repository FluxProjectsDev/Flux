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

// Parity harness: the legacy ProfilePolicy is used ONLY as a behavioural
// comparison fixture (never as the new implementation's template). For a set of
// safe steady-state scenarios it feeds identical inputs into the legacy engine
// and the V2 DecisionEngine (through the production compat mapping) and asserts
// they choose the same profile bucket.
//
// Intentional, documented differences (NOT asserted as equal here) — corrected
// safety semantics win over exact legacy parity:
//   * Restore settle: V2 defers an aggressive promotion for one healthy cycle
//     after telemetry returns; the legacy engine promotes immediately. Tested in
//     DecisionEngineTest; primed away here by warming both to a healthy cycle.
//   * Pressure debounce: V2 downgrades on thermal pressure immediately (a safety
//     move); the legacy engine gates thermal switches behind switch_debounce_ms.
//     Steady-state single-shot scenarios below are unaffected.
//   * Priority order: V2 ranks a thermal emergency above telemetry-offline; the
//     legacy engine ranks offline first. Unobservable, because an emergency needs
//     a valid reading, which an offline snapshot cannot provide.

#include "TestFramework.hpp"

#include "DecisionAdapter.hpp"
#include "DecisionEngine.hpp"

#include <ProfilePolicy.hpp>
#include <SynthesisCore.hpp>
#include <Flux.hpp>

#include <limits>
#include <optional>

using namespace flux::engine;

namespace {

enum class Bucket { Performance, Lite, Neutral, PowerSave };

Bucket bucket_of(FluxProfileMode m) {
    switch (m) {
        case PERFORMANCE_PROFILE: return Bucket::Performance;
        case PERFORMANCE_LITE_PROFILE: return Bucket::Lite;
        case POWERSAVE_PROFILE: return Bucket::PowerSave;
        case BALANCE_PROFILE:
        case PERFCOMMON:
            return Bucket::Neutral;
    }
    return Bucket::Neutral;
}

Bucket bucket_of(TargetProfile p) {
    switch (p) {
        case TargetProfile::Performance: return Bucket::Performance;
        case TargetProfile::PerformanceLite: return Bucket::Lite;
        case TargetProfile::PowerSave: return Bucket::PowerSave;
        case TargetProfile::Balanced: return Bucket::Neutral;
    }
    return Bucket::Neutral;
}

struct Scenario {
    const char *name;
    FluxProfileMode start;
    TelemetryHealth health;
    bool in_session;
    bool has_thermal;      // device supports thermal API
    bool thermal_valid;    // a usable reading this cycle
    float headroom;
    int thermal_status;
    bool battery_saver;
    bool screen_awake;
};

TelemetrySnapshot make_snapshot(const Scenario &sc) {
    TelemetrySnapshot s;
    s.schema_version = SYNTHESIS_SCHEMA_VERSION;
    s.thermal_available = sc.has_thermal;
    s.thermal_valid = sc.thermal_valid;
    s.thermal_headroom = sc.thermal_valid ? sc.headroom : std::numeric_limits<float>::quiet_NaN();
    s.thermal_status = sc.thermal_status;
    s.screen_available = true;
    s.screen_awake = sc.screen_awake;
    s.power_available = true;
    s.battery_saver = sc.battery_saver;
    s.charging_available = true;
    s.charging = false;
    s.audio_available = true;
    s.audio_active = false;
    return s;
}

Bucket legacy_choice(const Scenario &sc, int64_t now) {
    ProfilePolicy policy;
    PolicyState st;
    st.current = sc.start;
    st.had_telemetry = sc.health != TelemetryHealth::Offline;
    st.prev_screen_awake = true;

    PolicyInputs in;
    in.health = sc.health;
    in.snapshot = make_snapshot(sc);
    in.in_game_session = sc.in_session;
    in.active_package = sc.in_session ? "com.example.game" : "";
    in.game_forces_lite = false;

    return bucket_of(policy.evaluate(in, st, now).profile);
}

Bucket v2_choice(const Scenario &sc, int64_t now) {
    DecisionEngine engine;
    std::optional<TelemetrySnapshot> snap = make_snapshot(sc);
    DecisionInputs in;
    in.runtime = flux::engine::compat::build_runtime_snapshot(sc.health, snap);
    in.capabilities = flux::engine::compat::build_capabilities(snap);
    in.session.in_session = sc.in_session;
    in.session.package = sc.in_session ? "com.example.game" : "";

    EngineState st;
    st.current = flux::engine::compat::from_flux_profile(sc.start);
    st.initialized = true;
    st.prev_health = in.runtime.health; // warm: no restore-settle edge
    st.prev_in_session = sc.in_session;
    st.prev_screen_awake = true;

    return bucket_of(engine.evaluate(in, st, now).desired_profile);
}

} // namespace

TEST("parity: V2 and legacy agree on safe steady-state scenarios") {
    const int64_t now = 1'000'000; // far past every hysteresis window
    const int none = THERMAL_STATUS_NONE;
    const int severe = THERMAL_STATUS_SEVERE;
    const int critical = THERMAL_STATUS_CRITICAL;

    const Scenario scenarios[] = {
        {"cool game", PERFORMANCE_PROFILE, TelemetryHealth::Healthy, true, true, true, 0.30f, none, false, true},
        {"hot game (pressure)", PERFORMANCE_PROFILE, TelemetryHealth::Healthy, true, true, true, 0.90f, none, false, true},
        {"severe status", PERFORMANCE_PROFILE, TelemetryHealth::Healthy, true, true, true, 0.10f, severe, false, true},
        {"thermal emergency", PERFORMANCE_PROFILE, TelemetryHealth::Healthy, true, true, true, 1.30f, critical, false, true},
        {"battery saver in game", PERFORMANCE_PROFILE, TelemetryHealth::Healthy, true, true, true, 0.30f, none, true, true},
        {"screen off in game", PERFORMANCE_PROFILE, TelemetryHealth::Healthy, true, true, true, 0.30f, none, false, false},
        {"telemetry offline", PERFORMANCE_PROFILE, TelemetryHealth::Offline, true, true, false, 0.0f, none, false, true},
        {"idle healthy", BALANCE_PROFILE, TelemetryHealth::Healthy, false, true, true, 0.30f, none, false, true},
        {"cool game, no thermal API", BALANCE_PROFILE, TelemetryHealth::Healthy, true, false, false, 0.0f, none, false, true},
    };

    for (const auto &sc : scenarios) {
        const Bucket legacy = legacy_choice(sc, now);
        const Bucket v2 = v2_choice(sc, now);
        CHECK_MSG(legacy == v2,
                  std::string("parity mismatch in scenario: ") + sc.name +
                      " (legacy=" + std::to_string(static_cast<int>(legacy)) +
                      " v2=" + std::to_string(static_cast<int>(v2)) + ")");
    }
}

TEST("parity: the compat mapping round-trips every profile") {
    using namespace flux::engine::compat;
    const TargetProfile all[] = {TargetProfile::PowerSave, TargetProfile::Balanced,
                                 TargetProfile::PerformanceLite, TargetProfile::Performance};
    for (TargetProfile p : all) {
        CHECK(from_flux_profile(to_flux_profile(p)) == p);
    }
}
