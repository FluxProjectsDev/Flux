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

#include "PolicyIntent.hpp"

#include <algorithm>

namespace flux::execution {

using flux::engine::DataHealth;
using flux::engine::Decision;
using flux::engine::DecisionPriority;
using flux::engine::DecisionReason;
using flux::engine::TargetProfile;

const char *behavior_class_name(BehaviorClass behavior) {
    switch (behavior) {
        case BehaviorClass::Safe: return "safe";
        case BehaviorClass::PowerSave: return "powersave";
        case BehaviorClass::Balanced: return "balanced";
        case BehaviorClass::Interactive: return "interactive";
        case BehaviorClass::ConstrainedPerformance: return "constrained_performance";
        case BehaviorClass::SustainedPerformance: return "sustained_performance";
        case BehaviorClass::Restore: return "restore";
    }
    return "unknown";
}

const char *restore_scope_name(RestoreScope scope) {
    switch (scope) {
        case RestoreScope::None: return "none";
        case RestoreScope::ScreenOff: return "screen_off";
        case RestoreScope::SessionEnd: return "session_end";
        case RestoreScope::Shutdown: return "shutdown";
    }
    return "unknown";
}

const char *resolution_outcome_name(ResolutionOutcome outcome) {
    switch (outcome) {
        case ResolutionOutcome::Accepted: return "accepted";
        case ResolutionOutcome::ConstrainedBySafety: return "constrained_by_safety";
        case ResolutionOutcome::DeniedByPriority: return "denied_by_priority";
        case ResolutionOutcome::DeniedByCapability: return "denied_by_capability";
        case ResolutionOutcome::DeniedByDeviceValidation: return "denied_by_device_validation";
        case ResolutionOutcome::SkippedOptional: return "skipped_optional";
    }
    return "unknown";
}

std::string PolicyIntent::descriptor_key() const {
    // The intent vocabulary is richer than the descriptor vocabulary, deliberately. A node does
    // not care *why* Flux wants it conservative — screen off, battery saver and a shutdown all
    // mean the same thing to a GPU frequency cap. Collapsing here keeps packs from having to
    // enumerate policy concepts they have no opinion about.
    switch (behavior) {
        case BehaviorClass::SustainedPerformance: return "sustained_performance";
        case BehaviorClass::ConstrainedPerformance: return "constrained_performance";
        case BehaviorClass::Interactive:
        case BehaviorClass::Balanced: return "balanced";
        case BehaviorClass::Safe:
        case BehaviorClass::PowerSave:
        case BehaviorClass::Restore: return "safe";
    }
    return "safe";
}

// --- Decision -> PolicyIntent ----------------------------------------------

PolicyIntent IntentMapper::map(const Decision &decision) {
    PolicyIntent intent;
    intent.model_version = kPolicyIntentModelVersion;

    // Carried through, never recomputed: the Decision Engine already resolved the ladder.
    intent.source_profile = decision.desired_profile;
    intent.reason = decision.reason;
    intent.priority = decision.priority;
    intent.safety = decision.constraints;
    intent.health = decision.health;
    intent.safety_driven = decision.safety_driven;
    intent.diagnostic_context = decision.explanation;

    // --- behaviour class ---
    // Derived from the reason first and the profile second. The reason carries information the
    // profile has already thrown away: Balanced-because-thermal-emergency and
    // Balanced-because-nothing-is-happening are the same profile and very different intents,
    // and only one of them may be reconsidered by a later constraint.
    switch (decision.reason) {
        case DecisionReason::shutdown:
            intent.behavior = BehaviorClass::Restore;
            intent.restore = RestoreScope::Shutdown;
            break;
        case DecisionReason::screen_sleeping:
            intent.behavior = BehaviorClass::Restore;
            intent.restore = RestoreScope::ScreenOff;
            break;
        case DecisionReason::session_ended:
            intent.behavior = BehaviorClass::Restore;
            intent.restore = RestoreScope::SessionEnd;
            break;
        case DecisionReason::thermal_emergency:
            intent.behavior = BehaviorClass::Safe;
            break;
        default:
            switch (decision.desired_profile) {
                case TargetProfile::Performance: intent.behavior = BehaviorClass::SustainedPerformance; break;
                case TargetProfile::PerformanceLite: intent.behavior = BehaviorClass::ConstrainedPerformance; break;
                case TargetProfile::PowerSave: intent.behavior = BehaviorClass::PowerSave; break;
                case TargetProfile::Balanced: intent.behavior = BehaviorClass::Balanced; break;
            }
            break;
    }

    intent.intent_id = behavior_class_name(intent.behavior);
    intent.restoration_required = (intent.restore != RestoreScope::None);

    // --- what the ask permits ---
    // Promotion is permitted only when the decision actually asked to go up and nothing vetoed
    // it. `promotion_locked` is the Decision Engine's own statement that stale telemetry made a
    // promotion unsafe; honouring it here rather than re-testing staleness keeps one answer.
    const bool wants_performance = intent.behavior == BehaviorClass::SustainedPerformance ||
                                   intent.behavior == BehaviorClass::ConstrainedPerformance;
    intent.aggressive_promotion_permitted =
        wants_performance && !decision.constraints.promotion_locked && !decision.safety_driven &&
        decision.health == DataHealth::Healthy;

    // A safety response must not be negotiated with. Anything at or above TelemetrySafety on
    // the ladder is a downgrade Flux owes the user immediately.
    intent.immediate_downgrade_required =
        decision.safety_driven || decision.priority <= DecisionPriority::TelemetrySafety;

    return intent;
}

// --- PolicyIntent -> CapabilityIntentSet -----------------------------------

namespace {

CapabilityIntent make(std::string capability_id, std::string semantic_value, CapabilityGroup group,
                      const PolicyIntent &policy, bool critical, std::string conflict_group) {
    CapabilityIntent intent;
    intent.capability_id = std::move(capability_id);
    intent.semantic_value = std::move(semantic_value);
    intent.group = group;
    intent.source_intent_id = policy.intent_id;
    intent.priority = policy.priority;
    intent.critical = critical;
    intent.restoration_expected = policy.restoration_required;
    intent.reason = flux::engine::decision_reason_name(policy.reason);
    intent.conflict_group = std::move(conflict_group);
    intent.dependency_group = intent.conflict_group;
    intent.descriptor_key = policy.descriptor_key();
    return intent;
}

} // namespace

CapabilityIntentSet IntentMapper::expand(const PolicyIntent &policy) {
    CapabilityIntentSet set;
    set.policy = policy;

    const auto note = [&](const std::string &capability_id, ResolutionOutcome outcome,
                          std::string detail) {
        set.trace.push_back({capability_id, outcome, policy.priority, std::move(detail)});
    };

    // Semantic values, not mechanisms. "sustained" is a statement about what the CPU should
    // prioritise; which node expresses that is the descriptors' problem.
    const std::string cpu_class = [&] {
        switch (policy.behavior) {
            case BehaviorClass::SustainedPerformance: return "sustained";
            case BehaviorClass::ConstrainedPerformance: return "constrained";
            case BehaviorClass::Interactive: return "interactive";
            case BehaviorClass::PowerSave: return "efficient";
            case BehaviorClass::Safe:
            case BehaviorClass::Restore:
            case BehaviorClass::Balanced: return "balanced";
        }
        return "balanced";
    }();

    // --- CPU ---
    // Critical for an aggressive ask: a "performance" profile that could not touch the CPU is
    // not the profile the user was told they got. For everything else the CPU is optional —
    // a device that cannot be made *more* conservative is not a safety failure, it is a device
    // that was already conservative.
    set.intents.push_back(make("cpu.performance_class", cpu_class, CapabilityGroup::CpuPolicy,
                               policy, policy.behavior == BehaviorClass::SustainedPerformance,
                               "cpu"));

    // --- GPU ---
    set.intents.push_back(make("gpu.performance_class", cpu_class, CapabilityGroup::GpuPolicy,
                               policy, false, "gpu"));

    // --- memory / scheduler ---
    // Latency bias only means anything when something is actively asking for throughput.
    if (policy.behavior == BehaviorClass::SustainedPerformance ||
        policy.behavior == BehaviorClass::ConstrainedPerformance ||
        policy.behavior == BehaviorClass::Interactive) {
        set.intents.push_back(make("memory.latency_bias", "low_latency", CapabilityGroup::Memory,
                                   policy, false, "memory"));
        set.intents.push_back(make("scheduler.interactivity", "responsive",
                                   CapabilityGroup::Scheduler, policy, false, "scheduler"));
    } else {
        set.intents.push_back(make("memory.latency_bias", "balanced", CapabilityGroup::Memory,
                                   policy, false, "memory"));
    }

    // --- vendor ---
    // Always expressed, never assumed. The probe decides whether any device can honour it, and
    // it is optional by construction: a device without vendor knobs is a device without vendor
    // knobs, not a failure.
    set.intents.push_back(make("device.vendor_performance_mode", cpu_class,
                               CapabilityGroup::DeviceSpecific, policy, false, "vendor"));

    // --- zen ---
    if (policy.zen.change_requested || policy.zen.restoration_required) {
        auto zen = make("zen.temporary_mode", std::to_string(policy.zen.desired_mode),
                        CapabilityGroup::Zen, policy, false, "zen");
        zen.restoration_expected = policy.zen.restoration_required;
        set.intents.push_back(std::move(zen));
    }

    // --- restoration ---
    if (policy.restoration_required) {
        auto restore = make("restore.original_state", restore_scope_name(policy.restore),
                            CapabilityGroup::PowerMode, policy, false, "restore");
        restore.restoration_expected = true;
        set.intents.push_back(std::move(restore));
        note("restore.original_state", ResolutionOutcome::Accepted,
             std::string("restoration requested: ") + restore_scope_name(policy.restore));
    }

    // --- safety annotations on the trace ---
    // These do not change what is asked for — the Decision Engine already applied them — they
    // record *why* the ask is what it is, so a user asking "why am I not on performance" gets
    // an answer rather than a shrug.
    if (policy.safety.promotion_locked) {
        note("cpu.performance_class", ResolutionOutcome::ConstrainedBySafety,
             "telemetry is stale; the decision engine vetoed promotion");
    }
    if (policy.safety.thermal_unsupported) {
        note("cpu.performance_class", ResolutionOutcome::ConstrainedBySafety,
             "no thermal capability; thermal rules did not run");
    }
    if (policy.safety.audio_guard_active) {
        note("cpu.performance_class", ResolutionOutcome::ConstrainedBySafety,
             "audio is active; a cosmetic promotion was held");
    }
    if (policy.safety_driven) {
        note("cpu.performance_class", ResolutionOutcome::ConstrainedBySafety,
             std::string("safety-driven decision: ") +
                 flux::engine::decision_reason_name(policy.reason));
    }

    // Deterministic order, independent of how the intents were appended. Sorting by
    // (priority, capability_id) means two callers that build the same set in different orders
    // get byte-identical plans — which is what makes a plan diffable and a no-op detectable.
    std::stable_sort(set.intents.begin(), set.intents.end(),
                     [](const CapabilityIntent &a, const CapabilityIntent &b) {
                         if (a.priority != b.priority) return a.priority < b.priority;
                         return a.capability_id < b.capability_id;
                     });

    return set;
}

const CapabilityIntent *CapabilityIntentSet::find(const std::string &capability_id) const {
    const auto it = std::find_if(intents.begin(), intents.end(), [&](const CapabilityIntent &i) {
        return i.capability_id == capability_id;
    });
    return it == intents.end() ? nullptr : &*it;
}

bool CapabilityIntentSet::has_critical() const {
    return std::any_of(intents.begin(), intents.end(),
                       [](const CapabilityIntent &i) { return i.critical; });
}

} // namespace flux::execution
