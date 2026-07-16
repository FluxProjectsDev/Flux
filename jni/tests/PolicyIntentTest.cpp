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

// Decision -> PolicyIntent -> CapabilityIntentSet.
//
// Pure mapping: no device, no filesystem, no clock. These tests assert that the mapper
// *preserves* the Decision Engine's resolution rather than re-deriving it — a second
// implementation of the priority ladder is exactly what Stage 1 removed.

#include "TestFramework.hpp"

#include "PolicyIntent.hpp"

#include <algorithm>
#include <string>

using namespace flux::execution;
using flux::engine::DataHealth;
using flux::engine::Decision;
using flux::engine::DecisionPriority;
using flux::engine::DecisionReason;
using flux::engine::TargetProfile;

namespace {

Decision decide(TargetProfile profile, DecisionReason reason, DecisionPriority priority,
                DataHealth health = DataHealth::Healthy) {
    Decision d;
    d.desired_profile = profile;
    d.reason = reason;
    d.priority = priority;
    d.health = health;
    return d;
}

bool has_intent(const CapabilityIntentSet &set, const std::string &id) {
    return set.find(id) != nullptr;
}

} // namespace

TEST("policy intent: a healthy cool game maps to sustained performance") {
    const auto intent = IntentMapper::map(decide(TargetProfile::Performance,
                                                 DecisionReason::session_started,
                                                 DecisionPriority::SessionLifecycle));
    CHECK_EQ(intent.behavior, BehaviorClass::SustainedPerformance);
    CHECK_EQ(intent.intent_id, std::string("sustained_performance"));
    CHECK_EQ(intent.descriptor_key(), std::string("sustained_performance"));
    CHECK_MSG(intent.aggressive_promotion_permitted,
              "a healthy, unvetoed performance decision may promote");
    CHECK(!intent.immediate_downgrade_required);
    CHECK_EQ(intent.model_version, kPolicyIntentModelVersion);
}

TEST("policy intent: thermal pressure maps to constrained, not sustained") {
    const auto intent = IntentMapper::map(decide(TargetProfile::PerformanceLite,
                                                 DecisionReason::thermal_pressure,
                                                 DecisionPriority::SessionLifecycle));
    CHECK_EQ(intent.behavior, BehaviorClass::ConstrainedPerformance);
    CHECK_EQ(intent.descriptor_key(), std::string("constrained_performance"));
}

TEST("policy intent: a thermal emergency is safe and immediate, whatever the profile") {
    auto d = decide(TargetProfile::Balanced, DecisionReason::thermal_emergency,
                    DecisionPriority::ThermalEmergency);
    d.safety_driven = true;
    const auto intent = IntentMapper::map(d);

    CHECK_EQ(intent.behavior, BehaviorClass::Safe);
    CHECK_EQ(intent.descriptor_key(), std::string("safe"));
    CHECK_MSG(intent.immediate_downgrade_required,
              "a safety response must not wait for hysteresis or an audio guard");
    CHECK_MSG(!intent.aggressive_promotion_permitted, "safety never promotes");
}

TEST("policy intent: the reason distinguishes two identical profiles") {
    // Balanced-because-thermal-emergency and Balanced-because-idle are the same profile and
    // very different intents. Mapping on the profile alone would lose that.
    auto emergency = decide(TargetProfile::Balanced, DecisionReason::thermal_emergency,
                            DecisionPriority::ThermalEmergency);
    emergency.safety_driven = true;
    const auto idle = decide(TargetProfile::Balanced, DecisionReason::no_transition,
                             DecisionPriority::NoncriticalPreference);

    CHECK_EQ(IntentMapper::map(emergency).behavior, BehaviorClass::Safe);
    CHECK_EQ(IntentMapper::map(idle).behavior, BehaviorClass::Balanced);
}

TEST("policy intent: battery saver maps to powersave and never promotes") {
    const auto intent = IntentMapper::map(decide(TargetProfile::PowerSave,
                                                 DecisionReason::battery_saver_enabled,
                                                 DecisionPriority::BatterySaver));
    CHECK_EQ(intent.behavior, BehaviorClass::PowerSave);
    CHECK_EQ(intent.descriptor_key(), std::string("safe"));
    CHECK(!intent.aggressive_promotion_permitted);
}

TEST("policy intent: screen off is a restoration, not merely a downgrade") {
    // The distinction matters: screen-off must give back what was borrowed, not just pick a
    // lower profile and leave the borrowed state in place.
    const auto intent = IntentMapper::map(decide(TargetProfile::Balanced,
                                                 DecisionReason::screen_sleeping,
                                                 DecisionPriority::ScreenOff));
    CHECK_EQ(intent.behavior, BehaviorClass::Restore);
    CHECK_EQ(intent.restore, RestoreScope::ScreenOff);
    CHECK(intent.restoration_required);
    CHECK(!intent.aggressive_promotion_permitted);
}

TEST("policy intent: session end restores") {
    const auto intent = IntentMapper::map(decide(TargetProfile::Balanced,
                                                 DecisionReason::session_ended,
                                                 DecisionPriority::SessionLifecycle));
    CHECK_EQ(intent.restore, RestoreScope::SessionEnd);
    CHECK(intent.restoration_required);
}

TEST("policy intent: shutdown restores and is immediate") {
    auto d = decide(TargetProfile::Balanced, DecisionReason::shutdown,
                    DecisionPriority::ShutdownOrFatal);
    d.safety_driven = true;
    const auto intent = IntentMapper::map(d);

    CHECK_EQ(intent.restore, RestoreScope::Shutdown);
    CHECK(intent.restoration_required);
    CHECK_MSG(intent.immediate_downgrade_required, "a departing daemon must leave nothing behind");
}

TEST("policy intent: stale telemetry vetoes promotion via the engine's own constraint") {
    // The mapper must honour the Decision Engine's promotion_locked rather than re-testing
    // staleness itself. Two implementations of "is this stale" would eventually disagree.
    auto d = decide(TargetProfile::Performance, DecisionReason::session_started,
                    DecisionPriority::SessionLifecycle, DataHealth::Stale);
    d.constraints.promotion_locked = true;
    const auto intent = IntentMapper::map(d);

    CHECK_MSG(!intent.aggressive_promotion_permitted,
              "a promotion the engine vetoed must not be permitted here");
    CHECK(intent.safety.promotion_locked);
}

TEST("policy intent: offline telemetry never permits aggressive promotion") {
    const auto intent = IntentMapper::map(decide(TargetProfile::Performance,
                                                 DecisionReason::telemetry_offline,
                                                 DecisionPriority::TelemetrySafety,
                                                 DataHealth::Offline));
    CHECK(!intent.aggressive_promotion_permitted);
    CHECK_MSG(intent.immediate_downgrade_required,
              "telemetry safety is at the top of the ladder and must act at once");
}

TEST("policy intent: safety constraints are carried, not recomputed") {
    auto d = decide(TargetProfile::Performance, DecisionReason::session_started,
                    DecisionPriority::SessionLifecycle);
    d.constraints.audio_guard_active = true;
    d.constraints.thermal_unsupported = true;
    d.constraints.restore_settling = true;
    const auto intent = IntentMapper::map(d);

    CHECK(intent.safety.audio_guard_active);
    CHECK(intent.safety.thermal_unsupported);
    CHECK(intent.safety.restore_settling);
}

TEST("policy intent: identical decisions produce identical intents") {
    const auto a = IntentMapper::from_decision(decide(TargetProfile::Performance,
                                                      DecisionReason::session_started,
                                                      DecisionPriority::SessionLifecycle));
    const auto b = IntentMapper::from_decision(decide(TargetProfile::Performance,
                                                      DecisionReason::session_started,
                                                      DecisionPriority::SessionLifecycle));

    CHECK_EQ(a.intents.size(), b.intents.size());
    for (size_t i = 0; i < a.intents.size(); ++i) {
        CHECK_EQ(a.intents[i].capability_id, b.intents[i].capability_id);
        CHECK_EQ(a.intents[i].semantic_value, b.intents[i].semantic_value);
    }
}

TEST("policy intent: expansion order is deterministic and independent of construction") {
    // Sorted by (priority, capability_id), so a plan is diffable and a no-op is detectable.
    const auto set = IntentMapper::from_decision(decide(TargetProfile::Performance,
                                                        DecisionReason::session_started,
                                                        DecisionPriority::SessionLifecycle));
    for (size_t i = 1; i < set.intents.size(); ++i) {
        const auto &prev = set.intents[i - 1];
        const auto &cur = set.intents[i];
        const bool ordered = prev.priority < cur.priority ||
                             (prev.priority == cur.priority && prev.capability_id <= cur.capability_id);
        CHECK_MSG(ordered, "intents must be ordered by (priority, capability_id), found '" +
                               prev.capability_id + "' before '" + cur.capability_id + "'");
    }
}

TEST("policy intent: an aggressive ask makes the CPU critical, and a safe one does not") {
    // A "performance" profile that could not touch the CPU is not what the user was told they
    // got. A device that cannot be made *more* conservative is not a safety failure.
    const auto aggressive = IntentMapper::from_decision(decide(TargetProfile::Performance,
                                                               DecisionReason::session_started,
                                                               DecisionPriority::SessionLifecycle));
    const auto *cpu = aggressive.find("cpu.performance_class");
    CHECK(cpu != nullptr);
    if (cpu) CHECK_MSG(cpu->critical, "sustained performance requires the CPU");
    CHECK(aggressive.has_critical());

    const auto safe = IntentMapper::from_decision(decide(TargetProfile::Balanced,
                                                         DecisionReason::screen_sleeping,
                                                         DecisionPriority::ScreenOff));
    const auto *safe_cpu = safe.find("cpu.performance_class");
    CHECK(safe_cpu != nullptr);
    if (safe_cpu) CHECK_MSG(!safe_cpu->critical, "a conservative ask must not be critical");
}

TEST("policy intent: intents name outcomes, never paths or commands") {
    // The boundary that keeps the policy layer device-independent.
    for (auto reason : {DecisionReason::session_started, DecisionReason::thermal_emergency,
                        DecisionReason::screen_sleeping, DecisionReason::battery_saver_enabled,
                        DecisionReason::shutdown, DecisionReason::telemetry_offline}) {
        const auto set = IntentMapper::from_decision(
            decide(TargetProfile::Performance, reason, DecisionPriority::SessionLifecycle));
        for (const auto &intent : set.intents) {
            CHECK_MSG(intent.capability_id.find('/') == std::string::npos,
                      "capability id '" + intent.capability_id + "' looks like a path");
            CHECK_MSG(intent.semantic_value.find('/') == std::string::npos,
                      "semantic value '" + intent.semantic_value + "' looks like a path");
            for (const char *marker : {"$(", "`", ";", "|", "sh ", "echo "}) {
                CHECK_MSG(intent.semantic_value.find(marker) == std::string::npos,
                          "semantic value '" + intent.semantic_value + "' looks executable");
            }
        }
    }
}

TEST("policy intent: performance asks add latency and interactivity intents") {
    const auto perf = IntentMapper::from_decision(decide(TargetProfile::Performance,
                                                         DecisionReason::session_started,
                                                         DecisionPriority::SessionLifecycle));
    CHECK(has_intent(perf, "memory.latency_bias"));
    CHECK(has_intent(perf, "scheduler.interactivity"));
    CHECK(has_intent(perf, "device.vendor_performance_mode"));

    const auto *memory = perf.find("memory.latency_bias");
    if (memory) CHECK_EQ(memory->semantic_value, std::string("low_latency"));

    // An idle device has no reason to bias for latency.
    const auto idle = IntentMapper::from_decision(decide(TargetProfile::Balanced,
                                                         DecisionReason::no_transition,
                                                         DecisionPriority::NoncriticalPreference));
    CHECK(!has_intent(idle, "scheduler.interactivity"));
    const auto *idle_memory = idle.find("memory.latency_bias");
    if (idle_memory) CHECK_EQ(idle_memory->semantic_value, std::string("balanced"));
}

TEST("policy intent: a restore ask emits a restoration intent") {
    const auto set = IntentMapper::from_decision(decide(TargetProfile::Balanced,
                                                        DecisionReason::session_ended,
                                                        DecisionPriority::SessionLifecycle));
    const auto *restore = set.find("restore.original_state");
    CHECK(restore != nullptr);
    if (restore) {
        CHECK(restore->restoration_expected);
        CHECK_EQ(restore->semantic_value, std::string("session_end"));
    }
}

TEST("policy intent: the trace explains every safety constraint") {
    // A user asking "why am I not on performance" deserves an answer, not a shrug.
    auto d = decide(TargetProfile::PerformanceLite, DecisionReason::thermal_pressure,
                    DecisionPriority::SessionLifecycle, DataHealth::Stale);
    d.constraints.promotion_locked = true;
    d.constraints.audio_guard_active = true;
    const auto set = IntentMapper::expand(IntentMapper::map(d));

    const bool has_stale = std::any_of(set.trace.begin(), set.trace.end(), [](const auto &s) {
        return s.outcome == ResolutionOutcome::ConstrainedBySafety &&
               s.detail.find("stale") != std::string::npos;
    });
    const bool has_audio = std::any_of(set.trace.begin(), set.trace.end(), [](const auto &s) {
        return s.outcome == ResolutionOutcome::ConstrainedBySafety &&
               s.detail.find("audio") != std::string::npos;
    });
    CHECK_MSG(has_stale, "a stale-telemetry veto must appear in the trace");
    CHECK_MSG(has_audio, "an audio guard must appear in the trace");
}

// --- zen ---

TEST("policy intent: zen carries the exact mode, never a boolean") {
    // The legacy path collapsed zen to a bool, so restoring a user who had chosen total
    // silence or alarms-only silently rewrote them to "priority".
    for (int mode : {0, 1, 2, 3}) {
        auto policy = IntentMapper::map(decide(TargetProfile::Performance,
                                               DecisionReason::session_started,
                                               DecisionPriority::SessionLifecycle));
        policy.zen.change_requested = true;
        policy.zen.desired_mode = mode;
        policy.zen.restoration_required = true;
        policy.zen.original_mode = 3 - mode;

        const auto set = IntentMapper::expand(policy);
        const auto *zen = set.find("zen.temporary_mode");
        CHECK_MSG(zen != nullptr, "a zen request must produce a zen intent");
        if (zen) {
            CHECK_MSG(zen->semantic_value == std::to_string(mode),
                      "zen mode " + std::to_string(mode) + " must survive the mapping exactly, got '" +
                          zen->semantic_value + "'");
            CHECK(zen->restoration_expected);
        }
    }
}

TEST("policy intent: no zen request produces no zen intent") {
    const auto set = IntentMapper::from_decision(decide(TargetProfile::Performance,
                                                        DecisionReason::session_started,
                                                        DecisionPriority::SessionLifecycle));
    CHECK_MSG(set.find("zen.temporary_mode") == nullptr,
              "zen must not be touched when nothing asked for it");
}

TEST("policy intent: zen restoration defaults to yielding to an external user change") {
    // If the user changed zen themselves while Flux held it, Flux's captured original is stale
    // and writing it back would overwrite a live preference with an old one.
    const ZenIntent zen;
    CHECK_MSG(zen.external_change_blocks_restore,
              "the safe default is to leave a user's newer choice alone");
    CHECK(!zen.change_requested);
    CHECK(!zen.original_mode.has_value());
}
