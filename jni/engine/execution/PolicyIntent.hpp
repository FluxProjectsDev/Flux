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

#include <optional>
#include <string>
#include <vector>

#include "CapabilityState.hpp"
#include "DecisionEngine.hpp"
#include "DeviceDescriptor.hpp"

/**
 * @file PolicyIntent.hpp
 * @brief What Flux wants, expressed without reference to any device.
 *
 * Flux-owned (Category A).
 *
 * ## The layer this adds
 *
 * `Decision` says *which profile*. A descriptor says *which node and what value*. Between them
 * there has to be something that says *what we are trying to achieve*, or the two ends end up
 * knowing about each other — which is how the legacy applier ended up with profile names baked
 * into shell functions that wrote vendor nodes.
 *
 * A PolicyIntent contains no path, no node, no command, and no device. A CapabilityIntent names
 * an outcome ("cpu wants a sustained performance class"), never a mechanism. Everything about
 * *how* lives in descriptors, and everything about *whether* lives in the probe.
 *
 * ## This does not re-decide anything
 *
 * The Decision Engine's priority ladder already resolves every conflict the policy layer has —
 * shutdown, thermal emergency, telemetry safety, battery saver, screen off, session lifecycle,
 * charging, configured preference, audio guard, noncritical preference — and it is pure and
 * host-tested. This mapper **preserves** that outcome; it does not re-derive it. A second
 * implementation of the ladder here would be a second decision path, disagreeing with the first
 * on some input nobody tested, and Stage 1 removed the last one of those on purpose.
 *
 * What this layer adds is the constraint the Decision Engine cannot know about: what the
 * *device* can actually do. A decision to run Performance on hardware whose capabilities are
 * unavailable becomes a constrained intent here — never a quiet failure, and never a promotion.
 */
namespace flux::execution {

/// The intent model's own version. Intents cross a process boundary in future (Console,
/// Diagnostics), so they are versioned from the start rather than retrofitted.
inline constexpr int kPolicyIntentModelVersion = 1;

/**
 * @brief The class of behaviour an intent is asking for.
 *
 * Deliberately not `TargetProfile`. A profile is what the decision engine picked from a fixed
 * set; a behaviour class is what that pick *means*, and the two must be free to diverge — a
 * Policy Graph should be able to produce SustainedPerformance without there being a
 * `TargetProfile::Performance` to name it, and a future intent (thermal emergency restoration,
 * say) need not correspond to any profile at all.
 */
enum class BehaviorClass {
    Safe,                   ///< the most conservative state Flux will ask for
    PowerSave,              ///< explicit user power choice
    Balanced,               ///< no reason to do anything special
    Interactive,            ///< responsiveness matters; sustained throughput does not
    ConstrainedPerformance, ///< performance wanted, but bounded (thermal pressure, degraded data)
    SustainedPerformance,   ///< the full ask: a healthy, cool, foregrounded session
    Restore,                ///< put back what was found; not a performance state at all
};

const char *behavior_class_name(BehaviorClass behavior);

/** Why a restoration is being asked for. Restores are not interchangeable. */
enum class RestoreScope {
    None,
    ScreenOff,  ///< nobody is looking; give back what was borrowed
    SessionEnd, ///< the game exited
    Shutdown,   ///< the daemon is going away and must leave nothing behind
};

const char *restore_scope_name(RestoreScope scope);

/**
 * @brief A zen request, as an exact mode.
 *
 * Android's zen mode is an integer with four meanings (off / priority / none / alarms). The
 * legacy path collapsed it to a bool, so restoring a user who had chosen total silence or
 * alarms-only silently rewrote them to "priority". The mode is carried end to end.
 */
struct ZenIntent {
    bool change_requested = false;
    int desired_mode = 0;             ///< ZEN_MODE_*, the exact integer
    bool restoration_required = false;
    std::optional<int> original_mode; ///< what the user had; absent means it was never captured

    /// Restoration must yield to a newer user choice. If the user changed zen themselves while
    /// Flux held it, Flux's "original" is stale and writing it back would overwrite a live
    /// preference with an old one.
    bool external_change_blocks_restore = true;
};

/**
 * @brief One complete, device-independent statement of what Flux wants this cycle.
 *
 * Carries the decision it came from so every downstream rejection can be explained all the way
 * back to a reason and a priority, rather than surfacing as "it didn't work".
 */
struct PolicyIntent {
    int model_version = kPolicyIntentModelVersion;

    /// Stable identifier, also the key descriptors use in `policy_values`. A string rather than
    /// an enum so a pack can name an intent this build does not implement without failing to
    /// parse — it simply produces no action.
    std::string intent_id;

    BehaviorClass behavior = BehaviorClass::Balanced;
    RestoreScope restore = RestoreScope::None;

    // --- provenance of the ask ---
    flux::engine::TargetProfile source_profile = flux::engine::TargetProfile::Balanced;
    flux::engine::DecisionReason reason = flux::engine::DecisionReason::startup;
    flux::engine::DecisionPriority priority = flux::engine::DecisionPriority::NoncriticalPreference;
    flux::engine::SafetyConstraints safety;
    flux::engine::DataHealth health = flux::engine::DataHealth::Offline;

    // --- what the ask permits ---
    /// False whenever safety, staleness or an explicit user choice decided this. A planner may
    /// never raise aggression above what the decision allowed.
    bool aggressive_promotion_permitted = false;
    /// True when the ask is a safety response: it must not wait for hysteresis, an audio guard,
    /// or capability negotiation.
    bool immediate_downgrade_required = false;
    /// True when this intent's job is to give something back.
    bool restoration_required = false;
    /// True when the decision itself was safety-driven.
    bool safety_driven = false;

    ZenIntent zen;

    std::string diagnostic_context;

    /// The descriptor `policy_values` key this intent reads. The intent vocabulary is richer
    /// than the descriptor vocabulary on purpose: several intents legitimately mean the same
    /// thing to a node, and a descriptor should not have to enumerate policy concepts.
    [[nodiscard]] std::string descriptor_key() const;
};

/**
 * @brief One desired outcome, named semantically.
 *
 * No path, no node, no command. "cpu.performance_class = sustained" is a statement about what
 * is wanted; which nodes achieve it on this device is the descriptors' business, and whether
 * they can is the probe's.
 */
struct CapabilityIntent {
    std::string capability_id;   ///< semantic, e.g. "cpu.performance_class"
    std::string semantic_value;  ///< e.g. "sustained", "efficient", "restore"
    CapabilityGroup group = CapabilityGroup::DeviceSpecific;

    std::string source_intent_id;
    flux::engine::DecisionPriority priority = flux::engine::DecisionPriority::NoncriticalPreference;

    /// Critical means "the ask is not honoured without this". An unsupported critical intent
    /// prevents the aggressive plan; an unsupported optional one is a diagnostic.
    bool critical = false;
    bool restoration_expected = false;

    std::string reason;
    std::string conflict_group;   ///< two intents in one conflict group cannot both apply
    std::string dependency_group; ///< resolved together, succeed or fail together

    /// The descriptor key this intent reads, inherited from its PolicyIntent.
    std::string descriptor_key;
};

/** Why an intent was dropped or changed. Enumerated so diagnostics need not parse prose. */
enum class ResolutionOutcome {
    Accepted,
    ConstrainedBySafety,    ///< kept, but bounded (thermal, stale telemetry)
    DeniedByPriority,       ///< a higher-priority intent won its conflict group
    DeniedByCapability,     ///< the device cannot do it
    DeniedByDeviceValidation, ///< it could, but nobody has confirmed it on this hardware
    SkippedOptional,        ///< unsupported and not required
};

const char *resolution_outcome_name(ResolutionOutcome outcome);

/** One line of the explanation for how the ask became the plan. */
struct ResolutionStep {
    std::string capability_id;
    ResolutionOutcome outcome = ResolutionOutcome::Accepted;
    flux::engine::DecisionPriority priority = flux::engine::DecisionPriority::NoncriticalPreference;
    std::string detail;
};

/** The normalized set of outcomes Flux wants, plus how it got there. */
struct CapabilityIntentSet {
    PolicyIntent policy;
    std::vector<CapabilityIntent> intents;
    std::vector<ResolutionStep> trace;

    [[nodiscard]] const CapabilityIntent *find(const std::string &capability_id) const;
    [[nodiscard]] bool has_critical() const;
};

/**
 * @brief Turns a Decision into intents. Pure: no device, no filesystem, no clock.
 *
 * Deterministic by construction — same Decision in, same intents out, in the same order. The
 * planner sorts by (priority, capability_id) rather than by insertion, so the caller cannot
 * change the result by building inputs in a different order.
 */
class IntentMapper {
public:
    /// Map a resolved Decision onto a versioned PolicyIntent, preserving its reason, priority
    /// and safety constraints rather than re-deriving them.
    [[nodiscard]] static PolicyIntent map(const flux::engine::Decision &decision);

    /// Expand a PolicyIntent into the outcomes it implies.
    [[nodiscard]] static CapabilityIntentSet expand(const PolicyIntent &policy);

    /// Convenience: Decision -> intents in one step.
    [[nodiscard]] static CapabilityIntentSet from_decision(const flux::engine::Decision &decision) {
        return expand(map(decision));
    }
};

} // namespace flux::execution
