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

#include "DryRunPlanner.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace flux::execution {

using flux::engine::DataHealth;
using flux::engine::TargetProfile;

const char *execution_readiness_name(ExecutionReadiness readiness) {
    switch (readiness) {
        case ExecutionReadiness::Ready: return "ready";
        case ExecutionReadiness::Constrained: return "constrained";
        case ExecutionReadiness::CapabilityLimited: return "capability_limited";
        case ExecutionReadiness::Unsupported: return "unsupported";
        case ExecutionReadiness::DeviceValidationRequired: return "device_validation_required";
        case ExecutionReadiness::InvalidPlan: return "invalid_plan";
        case ExecutionReadiness::TelemetryDegraded: return "telemetry_degraded";
        case ExecutionReadiness::RestorationUnavailable: return "restoration_unavailable";
    }
    return "unknown";
}

DryRunExecutionPlan DryRunPlanner::plan(const CapabilityIntentSet &intents,
                                        const std::vector<DevicePack> &packs,
                                        const DeviceIdentity &identity,
                                        TargetProfile last_verified, int64_t now_ms) const {
    DryRunExecutionPlan plan;
    plan.requested = intents.policy;
    plan.trace = intents.trace;
    plan.projected.requested_profile = intents.policy.source_profile;
    plan.projected.requested_intent = intents.policy.intent_id;
    plan.projected.telemetry_health = intents.policy.health;
    // Carried through untouched. Nothing is applied here, so the last verified live profile is
    // exactly what it was — this is the single most important thing a dry run must not do.
    plan.projected.last_verified_profile = last_verified;

    const std::string key = intents.policy.descriptor_key();
    bool any_critical_denied = false;
    bool any_device_validation_pending = false;
    bool any_vendor_denied = false;
    bool any_generic_action = false;

    // --- resolve every intent against every candidate pack ---
    for (const auto &intent : intents.intents) {
        bool matched_any_descriptor = false;
        bool satisfied = false;

        for (const auto &pack : packs) {
            // Probe the pack once per intent key. The probe reads; it never writes and never
            // chmods, which is what makes this whole path safe to run on every cycle.
            const auto report = probe_.probe_pack(pack, identity, key, now_ms);

            for (size_t i = 0; i < pack.descriptors.size(); ++i) {
                const auto &descriptor = pack.descriptors[i];
                const auto &status = report.statuses[i];

                // A descriptor serves an intent when it belongs to the same group. Group is the
                // join, not the path: intents name outcomes, and several nodes can express one.
                if (descriptor.group != intent.group) continue;
                // A descriptor that says nothing about this intent key is not a candidate at
                // all — silence is not a failure and must not be reported as one.
                if (descriptor.policy_values.find(key) == descriptor.policy_values.end()) continue;

                matched_any_descriptor = true;

                if (!status.executable()) {
                    PreventedAction prevented;
                    prevented.capability_id = intent.capability_id;
                    prevented.descriptor_id = descriptor.descriptor_id;
                    prevented.descriptor_set = pack.pack_id;
                    prevented.state = status.state;
                    prevented.reason = status.reason;
                    prevented.was_critical = intent.critical;
                    prevented.detail = status.detail;
                    plan.prevented.push_back(std::move(prevented));

                    if (status.state == CapabilityState::DeviceValidationRequired) {
                        any_device_validation_pending = true;
                    }
                    if (descriptor.provenance == Provenance::DerivedEncore) any_vendor_denied = true;
                    // NOT any_critical_denied here. An intent is satisfied when *any* descriptor
                    // serves it, and several legitimately can: a vendor pack often adds a knob
                    // to the same outcome generic cpufreq already covers. Denying the intent
                    // because one of its descriptors is unavailable would withdraw the whole
                    // aggressive plan whenever a vendor extra was missing — which is every
                    // unvalidated device. Whether the intent went unserved is only knowable
                    // after every pack has been scanned; see below.

                    plan.trace.push_back({intent.capability_id,
                                          status.state == CapabilityState::DeviceValidationRequired
                                              ? ResolutionOutcome::DeniedByDeviceValidation
                                              : (intent.critical ? ResolutionOutcome::DeniedByCapability
                                                                 : ResolutionOutcome::SkippedOptional),
                                          intent.priority,
                                          pack.pack_id + "/" + descriptor.descriptor_id + ": " +
                                              capability_state_name(status.state)});
                    if (!intent.critical) ++plan.optional_skips;
                    continue;
                }

                // --- executable: emit a fully specified candidate action ---
                DryRunAction action;
                action.action_id = pack.pack_id + "." + descriptor.descriptor_id + "." + key;
                action.capability_id = intent.capability_id;
                action.descriptor_id = descriptor.descriptor_id;
                action.descriptor_set = pack.pack_id;
                action.backend = "sysfs";
                action.target_path = descriptor.path;
                action.desired_value = descriptor.policy_values.at(key);
                action.value_type = descriptor.value_type;
                action.read_back = descriptor.read_back;
                action.rollback = descriptor.rollback;
                action.rollback_value_available = status.rollback_available;
                action.order_group = descriptor.order_group;
                action.depends_on = descriptor.depends_on;
                action.conflict_group = intent.conflict_group;
                action.critical = intent.critical;
                action.source_intent_id = intent.source_intent_id;
                action.reason = intents.policy.reason;
                action.capability_state = status.state;
                action.validation = status.validation;

                if (action.rollback_value_available) ++plan.rollback_covered;
                if (descriptor.provenance == Provenance::FluxAuthored) any_generic_action = true;

                plan.actions.push_back(std::move(action));
                satisfied = true;
                plan.trace.push_back({intent.capability_id, ResolutionOutcome::Accepted,
                                      intent.priority,
                                      pack.pack_id + "/" + descriptor.descriptor_id});
            }
        }

        if (!matched_any_descriptor) {
            // No descriptor anywhere claims this outcome. For a critical intent that is a
            // capability failure; for an optional one it is simply a device without that knob.
            plan.trace.push_back({intent.capability_id,
                                  intent.critical ? ResolutionOutcome::DeniedByCapability
                                                  : ResolutionOutcome::SkippedOptional,
                                  intent.priority, "no descriptor serves this intent on this device"});
            if (intent.critical) any_critical_denied = true;
            else ++plan.optional_skips;
        } else if (!satisfied && intent.critical) {
            any_critical_denied = true;
        }
    }

    // --- deterministic ordering ---
    // (order_group, action_id). Never insertion order: a plan whose order depends on how the
    // packs happened to be enumerated is a plan that cannot be diffed against the last one, and
    // diffing is what makes a repeated decision a no-op instead of a rewrite.
    std::stable_sort(plan.actions.begin(), plan.actions.end(),
                     [](const DryRunAction &a, const DryRunAction &b) {
                         if (a.order_group != b.order_group) return a.order_group < b.order_group;
                         return a.action_id < b.action_id;
                     });

    // --- validation ---
    plan.valid = true;
    std::set<std::string> targets;
    std::set<std::string> action_ids;
    std::map<std::string, std::string> conflict_owner;

    for (const auto &action : plan.actions) {
        const auto invalidate = [&](const std::string &why) {
            plan.valid = false;
            if (plan.invalid_reason.empty()) plan.invalid_reason = why;
        };

        if (!action_ids.insert(action.action_id).second) {
            invalidate("duplicate action id: " + action.action_id);
        }
        // Two actions writing one node cannot both be right, and which wins would depend on
        // order.
        if (!targets.insert(action.target_path).second) {
            invalidate("two actions target the same node: " + action.target_path);
        }
        // Two intents in one conflict group asking for different things is a policy bug that
        // must not be resolved by luck.
        auto owner = conflict_owner.find(action.conflict_group);
        if (owner != conflict_owner.end() && owner->second != action.capability_id) {
            invalidate("conflicting intents in group '" + action.conflict_group + "': " +
                       owner->second + " vs " + action.capability_id);
        } else {
            conflict_owner[action.conflict_group] = action.capability_id;
        }

        // These should already be impossible — the descriptor validator and the probe both
        // refuse them — so reaching one means an earlier gate regressed. Checked anyway: this
        // is the last point before a real write in Increment 4.
        if (action.capability_state != CapabilityState::Supported) {
            invalidate("action for a non-supported capability: " + action.capability_id);
        }
        if (action.validation != ValidationStatus::PhysicalDeviceValidated) {
            invalidate("action for a descriptor not validated on hardware: " + action.descriptor_id);
        }
        if (action.critical && action.read_back == ReadBackStrategy::None) {
            invalidate("critical action without read-back: " + action.action_id);
        }
        if (action.rollback == RollbackStrategy::RestoreOriginal && !action.rollback_value_available) {
            invalidate("RestoreOriginal without a restorable original: " + action.action_id);
        }
        for (const auto &dep : action.depends_on) {
            const bool present = std::any_of(plan.actions.begin(), plan.actions.end(),
                                             [&](const DryRunAction &other) {
                                                 return other.descriptor_id == dep;
                                             });
            if (!present) invalidate("action depends on '" + dep + "', which is not in the plan");
        }
    }

    // An aggressive ask that lost a critical capability must not go out half-applied. Dropping
    // the critical action and applying the rest is the failure mode this rule exists to stop:
    // it leaves the device in a combination nobody designed and reports success.
    if (any_critical_denied) {
        plan.critical_rejection = true;
        const bool aggressive = intents.policy.behavior == BehaviorClass::SustainedPerformance;
        if (aggressive) {
            // Withdraw the whole aggressive plan, not just the critical action. Only an
            // aggressive ask is withdrawn this way — a safety downgrade is never blocked by a
            // missing performance capability, which is why this branch tests the behaviour
            // class rather than firing on any critical failure.
            const size_t before = plan.actions.size();
            plan.actions.clear();
            plan.trace.push_back({"cpu.performance_class", ResolutionOutcome::DeniedByCapability,
                                  intents.policy.priority,
                                  "aggressive plan withdrawn: a critical capability is not "
                                  "executable, and a partial aggressive plan is worse than none (" +
                                      std::to_string(before) + " candidate actions dropped)"});
        }
    }

    // --- projection ---
    auto &projected = plan.projected;
    projected.effective_intent = intents.policy.intent_id;
    projected.device_validation_pending = any_device_validation_pending;
    projected.rollback_fully_covered =
        !plan.actions.empty() && plan.rollback_covered == static_cast<int>(plan.actions.size());

    // Ordered by severity: the worst true statement wins, so a plan is never described as
    // better than it is.
    if (!plan.valid) {
        projected.readiness = ExecutionReadiness::InvalidPlan;
        projected.unsupported_reason = plan.invalid_reason;
    } else if (plan.critical_rejection) {
        projected.readiness = ExecutionReadiness::CapabilityLimited;
        projected.unsupported_reason = "a critical capability is not executable on this device";
    } else if (plan.actions.empty()) {
        projected.readiness = any_device_validation_pending
                                  ? ExecutionReadiness::DeviceValidationRequired
                                  : ExecutionReadiness::Unsupported;
        projected.unsupported_reason =
            any_device_validation_pending
                ? "every candidate awaits validation on real hardware of this family"
                : "no capability on this device serves the requested intent";
    } else if (intents.policy.health != DataHealth::Healthy) {
        projected.readiness = ExecutionReadiness::TelemetryDegraded;
        projected.degraded_reason = "the decision behind this plan rests on stale or offline data";
    } else if (any_vendor_denied && any_generic_action) {
        // The common real case: vendor tuning is unavailable or unvalidated, generic works.
        // Honest answer — the ask is partly met, and the plan says so rather than claiming the
        // profile is fully active.
        projected.readiness = ExecutionReadiness::CapabilityLimited;
        projected.unsupported_reason =
            "vendor capabilities are unavailable; only generic capabilities would be applied";
    } else if (intents.policy.safety.promotion_locked || intents.policy.safety_driven ||
               intents.policy.safety.audio_guard_active) {
        projected.readiness = ExecutionReadiness::Constrained;
        projected.degraded_reason = intents.policy.diagnostic_context;
    } else {
        projected.readiness = ExecutionReadiness::Ready;
    }

    if (intents.policy.restoration_required && !projected.rollback_fully_covered &&
        !plan.actions.empty()) {
        projected.readiness = ExecutionReadiness::RestorationUnavailable;
        projected.degraded_reason = "a restore was requested but not every action can give its "
                                    "original value back";
    }

    // "Fully active" is a strong claim, so it needs every part of the ask honoured — including
    // the optional parts. An optional intent that no descriptor serves is a real gap in the ask
    // even though it is not a failure: the device just cannot do that bit. Reporting the profile
    // as fully active anyway would tell the user their GPU was tuned by a plan that never had a
    // GPU descriptor to tune. Optional means "do not withdraw the plan over it", not "pretend it
    // happened". Note this is about what *would* happen: a dry run never reports a profile as
    // actually active, because it has not applied anything.
    projected.would_be_fully_active = plan.valid && !plan.critical_rejection &&
                                      !plan.actions.empty() && !any_critical_denied &&
                                      !any_vendor_denied && plan.optional_skips == 0 &&
                                      plan.prevented.empty() &&
                                      projected.readiness == ExecutionReadiness::Ready;

    return plan;
}

} // namespace flux::execution
