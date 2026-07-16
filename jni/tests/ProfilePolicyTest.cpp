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

#include "TestFramework.hpp"

#include <ProfilePolicy.hpp>

namespace {

/// A healthy snapshot of a cool device with a game in the foreground.
TelemetrySnapshot healthy_snapshot(float headroom = 0.30f, int status = THERMAL_STATUS_LIGHT) {
    TelemetrySnapshot s;
    s.schema_version       = 2;
    s.sequence             = 1;
    s.daemon_pid           = 1000;
    s.foreground_available = true;
    s.focused_package      = "com.example.game";
    s.focused_pid          = 900;
    s.focused_uid          = 10100;
    s.screen_available     = true;
    s.screen_awake         = true;
    s.power_available      = true;
    s.battery_saver        = false;
    s.charging_available   = true;
    s.charging             = false;
    s.thermal_available    = true;
    s.thermal_valid        = true;
    s.thermal_headroom     = headroom;
    s.thermal_status       = status;
    s.audio_available      = true;
    s.audio_active         = false;
    s.zen_available        = true;
    s.zen_mode             = 0;
    return s;
}

PolicyInputs game_inputs(const TelemetrySnapshot &s, TelemetryHealth health = TelemetryHealth::Healthy) {
    PolicyInputs in;
    in.health          = health;
    in.snapshot        = s;
    in.in_game_session = true;
    in.active_package  = "com.example.game";
    return in;
}

/// Drive the policy to a settled Performance state inside a game session.
PolicyState settled_performance(const ProfilePolicy &policy, int64_t &now) {
    PolicyState state;
    const auto snap = healthy_snapshot();
    (void)policy.evaluate(game_inputs(snap), state, now);
    now += 60'000; // move well past every debounce window
    (void)policy.evaluate(game_inputs(snap), state, now);
    return state;
}

} // namespace

// --- Thermal direction -------------------------------------------------------

/**
 * The headline correctness test.
 *
 * Android headroom is *higher when hotter*. The previous implementation downgraded when
 * headroom was LOW, i.e. when the device was cool, and restored full Performance when it was
 * hot. These two tests would both have failed against it.
 */
TEST("policy: a HIGH headroom means hot, and downgrades to Lite") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);
    CHECK_EQ(state.current, PERFORMANCE_PROFILE);

    now += 10'000;
    const auto decision = policy.evaluate(game_inputs(healthy_snapshot(0.90f)), state, now);

    CHECK_EQ(decision.profile, PERFORMANCE_LITE_PROFILE);
    CHECK_EQ(decision.reason, TransitionReason::ThermalPressure);
    CHECK(decision.safety_driven);
}

TEST("policy: a LOW headroom means cool, and stays in Performance") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    now += 10'000;
    const auto decision = policy.evaluate(game_inputs(healthy_snapshot(0.10f)), state, now);

    CHECK_EQ(decision.profile, PERFORMANCE_PROFILE);
}

// --- Hysteresis ---------------------------------------------------------------

TEST("policy: inside the hysteresis band the current tier is held") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    // 0.80 is between lite_exit (0.70) and lite_enter (0.85): neither hot enough to drop...
    now += 10'000;
    CHECK_EQ(policy.evaluate(game_inputs(healthy_snapshot(0.80f)), state, now).profile, PERFORMANCE_PROFILE);

    // ...now force into Lite, and confirm the same 0.80 does not let it climb back out.
    now += 10'000;
    CHECK_EQ(
        policy.evaluate(game_inputs(healthy_snapshot(0.90f)), state, now).profile, PERFORMANCE_LITE_PROFILE
    );

    now += 60'000; // past both debounce and recovery-hold
    CHECK_EQ(
        policy.evaluate(game_inputs(healthy_snapshot(0.80f)), state, now).profile, PERFORMANCE_LITE_PROFILE
    );
}

TEST("policy: recovery to Performance requires cooling past the exit threshold") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    now += 10'000;
    (void)policy.evaluate(game_inputs(healthy_snapshot(0.95f)), state, now);
    CHECK_EQ(state.current, PERFORMANCE_LITE_PROFILE);

    now += 60'000;
    const auto decision = policy.evaluate(game_inputs(healthy_snapshot(0.60f)), state, now);

    CHECK_EQ(decision.profile, PERFORMANCE_PROFILE);
    CHECK_EQ(decision.reason, TransitionReason::ThermalRecovered);
}

TEST("policy: recovery is held back until the recovery window elapses") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    now += 10'000;
    (void)policy.evaluate(game_inputs(healthy_snapshot(0.95f)), state, now);
    CHECK_EQ(state.current, PERFORMANCE_LITE_PROFILE);

    // Cool immediately, but well inside recovery_hold_ms (15s): must not bounce straight back.
    now += 2'000;
    CHECK_EQ(
        policy.evaluate(game_inputs(healthy_snapshot(0.20f)), state, now).profile, PERFORMANCE_LITE_PROFILE
    );

    now += 20'000;
    CHECK_EQ(policy.evaluate(game_inputs(healthy_snapshot(0.20f)), state, now).profile, PERFORMANCE_PROFILE);
}

TEST("policy: headroom oscillating on the threshold does not flap the profile") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    now += 10'000;
    (void)policy.evaluate(game_inputs(healthy_snapshot(0.90f)), state, now); // -> Lite
    CHECK_EQ(state.current, PERFORMANCE_LITE_PROFILE);

    int changes = 0;
    for (int i = 0; i < 40; ++i) {
        now += 500; // 20 s of 500 ms telemetry cycles
        const float headroom = (i % 2 == 0) ? 0.86f : 0.84f; // straddling lite_enter
        if (policy.evaluate(game_inputs(healthy_snapshot(headroom)), state, now).changed) ++changes;
    }

    CHECK_MSG(changes == 0, "hysteresis and debounce should prevent any flapping here");
    CHECK_EQ(state.current, PERFORMANCE_LITE_PROFILE);
}

// --- Emergency ----------------------------------------------------------------

TEST("policy: a thermal emergency drops out of the game profile immediately") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    now += 100; // deliberately inside the debounce window
    const auto decision = policy.evaluate(game_inputs(healthy_snapshot(1.30f)), state, now);

    CHECK_EQ(decision.profile, BALANCE_PROFILE);
    CHECK_EQ(decision.reason, TransitionReason::ThermalEmergency);
    CHECK_MSG(decision.safety_driven, "an emergency is a safety decision");
}

TEST("policy: a CRITICAL thermal status alone triggers an emergency") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    // Headroom looks benign, but the platform says critical. Trust the platform.
    auto snap = healthy_snapshot(0.20f, THERMAL_STATUS_CRITICAL);
    now += 10'000;

    CHECK_EQ(policy.evaluate(game_inputs(snap), state, now).reason, TransitionReason::ThermalEmergency);
}

TEST("policy: a SEVERE thermal status alone triggers a downgrade to Lite") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    auto snap = healthy_snapshot(0.20f, THERMAL_STATUS_SEVERE);
    now += 10'000;

    const auto decision = policy.evaluate(game_inputs(snap), state, now);
    CHECK_EQ(decision.profile, PERFORMANCE_LITE_PROFILE);
    CHECK_EQ(decision.reason, TransitionReason::ThermalPressure);
}

// --- The audio guard ----------------------------------------------------------

/**
 * The safety rule that was inverted in practice: the audio check used to run *first* and
 * return early, so a device playing game audio would remain in Performance at any
 * temperature whatsoever.
 */
TEST("policy: active audio does NOT block a thermal downgrade") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    auto snap         = healthy_snapshot(0.95f);
    snap.audio_active = true;
    now += 10'000;

    const auto decision = policy.evaluate(game_inputs(snap), state, now);
    CHECK_MSG(decision.profile == PERFORMANCE_LITE_PROFILE, "audio must never defer thermal safety");
    CHECK_EQ(decision.reason, TransitionReason::ThermalPressure);
}

TEST("policy: active audio does NOT block a thermal emergency") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    auto snap         = healthy_snapshot(1.40f);
    snap.audio_active = true;
    now += 10'000;

    CHECK_EQ(policy.evaluate(game_inputs(snap), state, now).profile, BALANCE_PROFILE);
}

TEST("policy: active audio does NOT block a battery-saver switch") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    auto snap          = healthy_snapshot();
    snap.audio_active  = true;
    snap.battery_saver = true;
    now += 10'000;

    CHECK_EQ(policy.evaluate(game_inputs(snap), state, now).profile, POWERSAVE_PROFILE);
}

// --- Priority ordering ---------------------------------------------------------

TEST("policy: battery saver overrides a game session") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    auto snap          = healthy_snapshot();
    snap.battery_saver = true;
    now += 10'000;

    const auto decision = policy.evaluate(game_inputs(snap), state, now);
    CHECK_EQ(decision.profile, POWERSAVE_PROFILE);
    CHECK_EQ(decision.reason, TransitionReason::BatterySaverEnabled);
}

TEST("policy: a thermal emergency outranks battery saver") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    auto snap          = healthy_snapshot(1.50f);
    snap.battery_saver = true;
    now += 10'000;

    CHECK_EQ(policy.evaluate(game_inputs(snap), state, now).reason, TransitionReason::ThermalEmergency);
}

TEST("policy: screen off leaves the game profile") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    auto snap         = healthy_snapshot();
    snap.screen_awake = false;
    now += 10'000;

    const auto decision = policy.evaluate(game_inputs(snap), state, now);
    CHECK_EQ(decision.profile, BALANCE_PROFILE);
    CHECK_EQ(decision.reason, TransitionReason::ScreenOff);
}

// --- Telemetry health ----------------------------------------------------------

TEST("policy: offline telemetry falls back to a safe profile") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    PolicyInputs in;
    in.health          = TelemetryHealth::Offline;
    in.snapshot        = std::nullopt;
    in.in_game_session = true;
    in.active_package  = "com.example.game";

    now += 10'000;
    const auto decision = policy.evaluate(in, state, now);

    CHECK_EQ(decision.profile, BALANCE_PROFILE);
    CHECK_EQ(decision.reason, TransitionReason::TelemetryOffline);
    CHECK(decision.safety_driven);
}

/** The prohibited behaviour, asserted directly. */
TEST("policy: stale telemetry can never promote performance") {
    const ProfilePolicy policy;
    int64_t now = 0;

    PolicyState state;
    state.current = BALANCE_PROFILE;

    // A snapshot that, if fresh, would select Performance.
    now += 10'000;
    const auto decision = policy.evaluate(game_inputs(healthy_snapshot(0.10f), TelemetryHealth::Stale), state, now);

    CHECK_MSG(decision.profile == BALANCE_PROFILE, "stale data must not raise the profile");
    CHECK_EQ(decision.reason, TransitionReason::TelemetryStale);
    CHECK(decision.safety_driven);
}

TEST("policy: stale telemetry still permits a downgrade") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    // Stale, but hot. Dropping the profile is always allowed.
    now += 10'000;
    const auto decision =
        policy.evaluate(game_inputs(healthy_snapshot(1.40f), TelemetryHealth::Stale), state, now);

    CHECK_EQ(decision.profile, BALANCE_PROFILE);
    CHECK_EQ(decision.reason, TransitionReason::ThermalEmergency);
}

TEST("policy: telemetry returning after an outage restores normal evaluation") {
    const ProfilePolicy policy;
    int64_t now = 0;
    PolicyState state;

    PolicyInputs offline;
    offline.health   = TelemetryHealth::Offline;
    offline.snapshot = std::nullopt;
    (void)policy.evaluate(offline, state, now);
    CHECK_EQ(state.current, BALANCE_PROFILE);

    now += 30'000;
    const auto decision = policy.evaluate(game_inputs(healthy_snapshot(0.10f)), state, now);

    CHECK_EQ(decision.profile, PERFORMANCE_PROFILE);
    CHECK_EQ(decision.reason, TransitionReason::TelemetryRestored);
}

// --- Thermal availability ------------------------------------------------------

TEST("policy: a device without the thermal API still reaches Performance") {
    const ProfilePolicy policy;
    int64_t now = 0;
    PolicyState state;

    auto snap              = healthy_snapshot();
    snap.thermal_available = false;
    snap.thermal_valid     = false;
    snap.thermal_headroom  = std::numeric_limits<float>::quiet_NaN();
    snap.thermal_status    = THERMAL_STATUS_UNKNOWN;

    now += 1000;
    CHECK_EQ(policy.evaluate(game_inputs(snap), state, now).profile, PERFORMANCE_PROFILE);
}

/** Absent thermal data is not evidence that it is safe to promote. */
TEST("policy: an invalid thermal sample holds Lite rather than promoting out of it") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    now += 10'000;
    (void)policy.evaluate(game_inputs(healthy_snapshot(0.95f)), state, now);
    CHECK_EQ(state.current, PERFORMANCE_LITE_PROFILE);

    // Thermal supported, but this sample is not usable.
    auto snap             = healthy_snapshot();
    snap.thermal_valid    = false;
    snap.thermal_headroom = std::numeric_limits<float>::quiet_NaN();

    now += 60'000;
    CHECK_MSG(
        policy.evaluate(game_inputs(snap), state, now).profile == PERFORMANCE_LITE_PROFILE,
        "must not promote on a thermal reading we do not have"
    );
}

// --- Config -------------------------------------------------------------------

TEST("policy: a config-forced Lite mode pins the tier even when cool") {
    const ProfilePolicy policy;
    int64_t now = 0;
    PolicyState state;

    auto in             = game_inputs(healthy_snapshot(0.05f));
    in.game_forces_lite = true;

    now += 1000;
    CHECK_EQ(policy.evaluate(in, state, now).profile, PERFORMANCE_LITE_PROFILE);
}

TEST("policy: shutdown wins over everything") {
    const ProfilePolicy policy;
    int64_t now = 0;
    auto state  = settled_performance(policy, now);

    auto in               = game_inputs(healthy_snapshot(0.05f));
    in.shutdown_requested = true;

    const auto decision = policy.evaluate(in, state, now);
    CHECK_EQ(decision.reason, TransitionReason::ShutdownRequested);
    CHECK_EQ(decision.profile, BALANCE_PROFILE);
}

TEST("policy: default thermal thresholds are self-consistent") {
    const ThermalThresholds defaults;
    CHECK_MSG(defaults.valid(), "lite_exit < lite_enter <= emergency must hold");
}

// --- Transition history --------------------------------------------------------

TEST("history: is bounded and keeps the most recent records") {
    TransitionHistory history(4);

    for (int i = 0; i < 10; ++i) {
        TransitionRecord record;
        record.from         = BALANCE_PROFILE;
        record.to           = PERFORMANCE_PROFILE;
        record.reason       = TransitionReason::GameStarted;
        record.monotonic_ms = i;
        history.record(record);
    }

    CHECK_EQ(history.size(), size_t{4});
    CHECK_EQ(history.records().front().monotonic_ms, 6LL);
    CHECK_EQ(history.records().back().monotonic_ms, 9LL);
}
