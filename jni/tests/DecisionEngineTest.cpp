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

// Host tests for the Flux V2 Decision Engine. Pure inputs, pure outputs — every
// priority, safety rule, and hysteresis path is asserted here without any I/O.

#include "TestFramework.hpp"

#include "DecisionEngine.hpp"

using namespace flux::engine;

namespace {

// Android thermal status shorthands used by the tests.
constexpr int kStatusNone = 0;
constexpr int kStatusSevere = 3;
constexpr int kStatusCritical = 4;

RuntimeSnapshot cool_healthy(float headroom = 0.30f) {
    RuntimeSnapshot rt;
    rt.health = DataHealth::Healthy;
    rt.thermal = ThermalReading{headroom, kStatusNone};
    rt.screen_awake = true;
    return rt;
}

CapabilitySnapshot with_thermal() { return CapabilitySnapshot{true}; }

DecisionInputs game(RuntimeSnapshot rt, bool forces_lite = false) {
    DecisionInputs in;
    in.runtime = std::move(rt);
    in.capabilities = with_thermal();
    in.session.in_session = true;
    in.session.package = "com.example.game";
    in.session.forces_lite = forces_lite;
    return in;
}

DecisionInputs idle(RuntimeSnapshot rt) {
    DecisionInputs in;
    in.runtime = std::move(rt);
    in.capabilities = with_thermal();
    return in;
}

/// A state already settled at @p profile with a healthy prior sample, so promotion
/// guards (which only fire on a health edge) do not interfere with the rule under test.
EngineState settled(TargetProfile profile, bool in_session = false) {
    EngineState s;
    s.current = profile;
    s.initialized = true;
    s.prev_health = DataHealth::Healthy;
    s.prev_in_session = in_session;
    s.prev_screen_awake = true;
    return s;
}

} // namespace

// --- Priorities ------------------------------------------------------------

TEST("decision: shutdown wins over everything") {
    DecisionEngine engine;
    DecisionInputs in = game(cool_healthy());
    in.shutdown_requested = true;
    in.runtime.battery_saver = true;
    in.runtime.thermal = ThermalReading{1.5f, kStatusCritical}; // even an emergency

    Decision d = engine.evaluate(in, settled(TargetProfile::Performance, true), 1000);
    CHECK(d.reason == DecisionReason::shutdown);
    CHECK(d.priority == DecisionPriority::ShutdownOrFatal);
    CHECK(d.desired_profile == TargetProfile::Balanced);
    CHECK(d.safety_driven);
}

TEST("decision: a thermal emergency outranks battery saver") {
    DecisionEngine engine;
    DecisionInputs in = game(cool_healthy());
    in.runtime.battery_saver = true;
    in.runtime.thermal = ThermalReading{1.20f, kStatusCritical};

    Decision d = engine.evaluate(in, settled(TargetProfile::Performance, true), 1000);
    CHECK(d.reason == DecisionReason::thermal_emergency);
    CHECK(d.priority == DecisionPriority::ThermalEmergency);
    CHECK(d.desired_profile == TargetProfile::Balanced);
}

TEST("decision: battery saver overrides a game session") {
    DecisionEngine engine;
    DecisionInputs in = game(cool_healthy());
    in.runtime.battery_saver = true;

    Decision d = engine.evaluate(in, settled(TargetProfile::Performance, true), 1000);
    CHECK(d.reason == DecisionReason::battery_saver_enabled);
    CHECK(d.desired_profile == TargetProfile::PowerSave);
    CHECK(d.safety_driven);
}

TEST("decision: screen off leaves the game profile") {
    DecisionEngine engine;
    DecisionInputs in = game(cool_healthy());
    in.runtime.screen_awake = false;

    Decision d = engine.evaluate(in, settled(TargetProfile::Performance, true), 1000);
    CHECK(d.reason == DecisionReason::screen_sleeping);
    CHECK(d.desired_profile == TargetProfile::Balanced);
    CHECK(d.safety_driven);
}

TEST("decision: telemetry offline falls back to a safe profile") {
    DecisionEngine engine;
    RuntimeSnapshot rt;
    rt.health = DataHealth::Offline;
    DecisionInputs in = game(rt);

    Decision d = engine.evaluate(in, settled(TargetProfile::Performance, true), 1000);
    CHECK(d.reason == DecisionReason::telemetry_offline);
    CHECK(d.desired_profile == TargetProfile::Balanced);
    CHECK(d.safety_driven);
}

// --- Session lifecycle -----------------------------------------------------

TEST("decision: a cool game session reaches Performance") {
    DecisionEngine engine;
    Decision d = engine.evaluate(game(cool_healthy()), settled(TargetProfile::Balanced), 1000);
    CHECK(d.desired_profile == TargetProfile::Performance);
    CHECK(d.reason == DecisionReason::session_started);
    CHECK(d.transition_required);
}

TEST("decision: leaving a session returns to Balanced with session_ended") {
    DecisionEngine engine;
    Decision d = engine.evaluate(idle(cool_healthy()), settled(TargetProfile::Performance, true), 1000);
    CHECK(d.desired_profile == TargetProfile::Balanced);
    CHECK(d.reason == DecisionReason::session_ended);
}

TEST("decision: a config-forced Lite mode pins the tier even when cool") {
    DecisionEngine engine;
    Decision d = engine.evaluate(game(cool_healthy(), /*forces_lite=*/true),
                                 settled(TargetProfile::Balanced), 1000);
    CHECK(d.desired_profile == TargetProfile::PerformanceLite);
}

// --- Thermal --------------------------------------------------------------

TEST("decision: thermal pressure downgrades a running game to Lite") {
    DecisionEngine engine;
    DecisionInputs in = game(cool_healthy(0.90f)); // >= lite_enter (0.85)
    Decision d = engine.evaluate(in, settled(TargetProfile::Performance, true), 10000);
    CHECK(d.desired_profile == TargetProfile::PerformanceLite);
    CHECK(d.reason == DecisionReason::thermal_pressure);
    CHECK(d.safety_driven);
}

TEST("decision: a SEVERE status alone triggers a pressure downgrade") {
    DecisionEngine engine;
    DecisionInputs in = game(cool_healthy(0.10f)); // float says cool
    in.runtime.thermal = ThermalReading{0.10f, kStatusSevere};
    Decision d = engine.evaluate(in, settled(TargetProfile::Performance, true), 10000);
    CHECK(d.desired_profile == TargetProfile::PerformanceLite);
    CHECK(d.reason == DecisionReason::thermal_pressure);
}

TEST("decision: a CRITICAL status alone triggers an emergency") {
    DecisionEngine engine;
    DecisionInputs in = game(cool_healthy(0.10f));
    in.runtime.thermal = ThermalReading{0.10f, kStatusCritical};
    Decision d = engine.evaluate(in, settled(TargetProfile::Performance, true), 10000);
    CHECK(d.reason == DecisionReason::thermal_emergency);
}

TEST("decision: headroom above 1.0 is an emergency, not clamped away") {
    DecisionEngine engine;
    DecisionInputs in = game(cool_healthy(1.30f)); // past severe threshold
    Decision d = engine.evaluate(in, settled(TargetProfile::Performance, true), 10000);
    CHECK(d.reason == DecisionReason::thermal_emergency);
    CHECK(d.desired_profile == TargetProfile::Balanced);
}

TEST("decision: inside the hysteresis band the current tier is held") {
    DecisionEngine engine;
    // 0.78 is between lite_exit (0.70) and lite_enter (0.85).
    Decision held_perf = engine.evaluate(game(cool_healthy(0.78f)),
                                         settled(TargetProfile::Performance, true), 10000);
    CHECK(held_perf.desired_profile == TargetProfile::Performance);
    CHECK(held_perf.reason == DecisionReason::no_transition);

    Decision held_lite = engine.evaluate(game(cool_healthy(0.78f)),
                                         settled(TargetProfile::PerformanceLite, true), 10000);
    CHECK(held_lite.desired_profile == TargetProfile::PerformanceLite);
}

TEST("decision: recovery to Performance is held back until the recovery window elapses") {
    DecisionEngine engine;
    EngineState s = settled(TargetProfile::PerformanceLite, true);
    s.last_thermal_switch_ms = 10000;

    // Cool now, but not enough time has passed (recovery_hold_ms = 15000).
    Decision early = engine.evaluate(game(cool_healthy(0.50f)), s, 10000 + 14999);
    CHECK(early.desired_profile == TargetProfile::PerformanceLite);
    CHECK(early.reason == DecisionReason::no_transition);

    // Window elapsed: recovery is now permitted.
    Decision late = engine.evaluate(game(cool_healthy(0.50f)), s, 10000 + 15000);
    CHECK(late.desired_profile == TargetProfile::Performance);
    CHECK(late.reason == DecisionReason::thermal_recovered);
    CHECK(late.recovery_allowed);
}

TEST("decision: an invalid thermal sample holds Lite rather than promoting out of it") {
    DecisionEngine engine;
    RuntimeSnapshot rt;
    rt.health = DataHealth::Healthy;
    rt.thermal = std::nullopt; // NaN was rejected upstream: no reading this cycle
    DecisionInputs in = game(rt);

    Decision d = engine.evaluate(in, settled(TargetProfile::PerformanceLite, true), 30000);
    CHECK(d.desired_profile == TargetProfile::PerformanceLite);
    CHECK(d.reason == DecisionReason::no_transition);
}

TEST("decision: a device without the thermal API still reaches Performance") {
    DecisionEngine engine;
    RuntimeSnapshot rt;
    rt.health = DataHealth::Healthy;
    rt.thermal = std::nullopt;
    DecisionInputs in;
    in.runtime = rt;
    in.capabilities.thermal_supported = false; // no thermal API at all
    in.session.in_session = true;

    Decision d = engine.evaluate(in, settled(TargetProfile::Balanced), 1000);
    CHECK(d.desired_profile == TargetProfile::Performance);
    CHECK(d.constraints.thermal_unsupported);
}

// --- Audio guard -----------------------------------------------------------

TEST("decision: active audio does NOT block a thermal downgrade") {
    DecisionEngine engine;
    DecisionInputs in = game(cool_healthy(0.90f));
    in.runtime.audio_active = true;
    Decision d = engine.evaluate(in, settled(TargetProfile::Performance, true), 10000);
    CHECK(d.desired_profile == TargetProfile::PerformanceLite);
    CHECK(d.reason == DecisionReason::thermal_pressure);
}

TEST("decision: active audio does NOT block a thermal emergency") {
    DecisionEngine engine;
    DecisionInputs in = game(cool_healthy(1.30f));
    in.runtime.audio_active = true;
    Decision d = engine.evaluate(in, settled(TargetProfile::Performance, true), 10000);
    CHECK(d.reason == DecisionReason::thermal_emergency);
}

TEST("decision: active audio does NOT block a battery-saver switch") {
    DecisionEngine engine;
    DecisionInputs in = game(cool_healthy());
    in.runtime.audio_active = true;
    in.runtime.battery_saver = true;
    Decision d = engine.evaluate(in, settled(TargetProfile::Performance, true), 10000);
    CHECK(d.desired_profile == TargetProfile::PowerSave);
}

TEST("decision: active audio holds back a cosmetic recovery promotion") {
    DecisionEngine engine;
    EngineState s = settled(TargetProfile::PerformanceLite, true);
    s.last_thermal_switch_ms = 0;
    DecisionInputs in = game(cool_healthy(0.50f)); // cool, recovery window long past
    in.runtime.audio_active = true;

    Decision d = engine.evaluate(in, s, 100000);
    CHECK(d.desired_profile == TargetProfile::PerformanceLite);
    CHECK(d.reason == DecisionReason::audio_hold);
    CHECK(d.constraints.audio_guard_active);
}

// --- Stale / restore -------------------------------------------------------

TEST("decision: stale telemetry can never promote performance") {
    DecisionEngine engine;
    RuntimeSnapshot rt = cool_healthy();
    rt.health = DataHealth::Stale;
    Decision d = engine.evaluate(game(rt), settled(TargetProfile::Balanced), 1000);
    CHECK(d.desired_profile == TargetProfile::Balanced);
    CHECK(d.reason == DecisionReason::telemetry_stale);
    CHECK(d.constraints.promotion_locked);
}

TEST("decision: stale telemetry still permits a downgrade") {
    DecisionEngine engine;
    RuntimeSnapshot rt = cool_healthy();
    rt.health = DataHealth::Stale;
    rt.battery_saver = true; // a downgrade
    Decision d = engine.evaluate(game(rt), settled(TargetProfile::Performance, true), 1000);
    CHECK(d.desired_profile == TargetProfile::PowerSave);
    CHECK(d.reason == DecisionReason::battery_saver_enabled);
}

TEST("decision: performance is not promoted the instant telemetry is restored") {
    DecisionEngine engine;
    EngineState s = settled(TargetProfile::Balanced, true);
    s.prev_health = DataHealth::Offline; // was offline; healthy now

    Decision first = engine.evaluate(game(cool_healthy()), s, 1000);
    CHECK(first.desired_profile == TargetProfile::Balanced);
    CHECK(first.reason == DecisionReason::telemetry_restored);
    CHECK(first.constraints.restore_settling);

    // Second healthy cycle: settled, promotion now allowed.
    Decision second = engine.evaluate(game(cool_healthy()), first.next_state, 1500);
    CHECK(second.desired_profile == TargetProfile::Performance);
}

// --- Charging --------------------------------------------------------------

TEST("decision: a charging transition while idle is reported without flapping") {
    DecisionEngine engine;
    RuntimeSnapshot rt = cool_healthy();
    rt.charging = true;
    EngineState s = settled(TargetProfile::Balanced);
    s.prev_charging = false;

    Decision d = engine.evaluate(idle(rt), s, 1000);
    CHECK(d.desired_profile == TargetProfile::Balanced);
    CHECK(d.reason == DecisionReason::charging_policy);
    CHECK(d.priority == DecisionPriority::ChargingPolicy);
}

// --- Determinism, boundaries, flapping, startup ---------------------------

TEST("decision: repeated evaluation with identical inputs is identical") {
    DecisionEngine engine;
    DecisionInputs in = game(cool_healthy(0.90f));
    EngineState s = settled(TargetProfile::Performance, true);

    Decision a = engine.evaluate(in, s, 12345);
    Decision b = engine.evaluate(in, s, 12345);
    CHECK(a.desired_profile == b.desired_profile);
    CHECK(a.reason == b.reason);
    CHECK(a.priority == b.priority);
    CHECK(a.transition_required == b.transition_required);
}

TEST("decision: the lite_enter boundary value is treated as pressure") {
    DecisionEngine engine;
    // Exactly lite_enter (0.85): the comparison is >=, so this is pressure.
    Decision d = engine.evaluate(game(cool_healthy(0.85f)),
                                 settled(TargetProfile::Performance, true), 10000);
    CHECK(d.desired_profile == TargetProfile::PerformanceLite);
}

TEST("decision: headroom oscillating on the threshold does not flap the profile") {
    DecisionEngine engine;
    EngineState s = settled(TargetProfile::Performance, true);
    int transitions = 0;
    int64_t now = 10000;
    const float samples[] = {0.86f, 0.84f, 0.86f, 0.84f, 0.83f, 0.86f};
    for (float h : samples) {
        Decision d = engine.evaluate(game(cool_healthy(h)), s, now);
        if (d.transition_required) ++transitions;
        s = d.next_state;
        now += 1000;
    }
    // One downgrade into Lite; the band then holds it. No upward flapping.
    CHECK_EQ(transitions, 1);
    CHECK(s.current == TargetProfile::PerformanceLite);
}

TEST("decision: safe startup with no previous state and no telemetry") {
    DecisionEngine engine;
    RuntimeSnapshot rt; // defaults: Offline
    DecisionInputs in;
    in.runtime = rt;

    EngineState fresh; // initialized == false
    Decision d = engine.evaluate(in, fresh, 0);
    CHECK(d.desired_profile == TargetProfile::Balanced);
    CHECK(d.reason == DecisionReason::telemetry_offline);
    CHECK(d.next_state.initialized);
}

TEST("decision: a healthy idle device sits at Balanced without churn") {
    DecisionEngine engine;
    Decision d = engine.evaluate(idle(cool_healthy()), settled(TargetProfile::Balanced), 1000);
    CHECK(d.desired_profile == TargetProfile::Balanced);
    CHECK(d.reason == DecisionReason::no_transition);
    CHECK(!d.transition_required);
}
