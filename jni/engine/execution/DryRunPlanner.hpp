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

#include <cstdint>
#include <string>
#include <vector>

#include "DeviceDescriptor.hpp"
#include "PolicyIntent.hpp"

/**
 * @file DryRunPlanner.hpp
 * @brief Compiles intents plus probe results into a plan that is never executed.
 *
 * Flux-owned (Category A).
 *
 * ## Why a dry run is its own thing
 *
 * Planning and applying are separated because they fail differently and are trusted
 * differently. A plan can be computed, inspected, diffed, shown in a UI and tested exhaustively
 * on a laptop; applying it needs root and a real device. The legacy applier fused the two —
 * deciding, planning and writing happened in the same shell function — which is why nothing
 * about it could be checked before it ran on someone's phone.
 *
 * This planner produces exactly what the live engine will need, and stops. Nothing here writes,
 * chmods, forks, or touches a device beyond the reads the probe already did. Increment 4 adds
 * execution *behind* this interface; it does not re-plan.
 *
 * ## The honest-projection rule
 *
 * A dry run reports what *would* happen, including all the ways it would fall short. It never
 * reports a profile as active — it has not applied anything — and it never upgrades an ask to
 * fill a gap. If the vendor pack cannot act, the plan says CapabilityLimited and shows which
 * generic actions survive; it does not quietly claim the profile worked.
 */
namespace flux::execution {

/** How ready the runtime would be if this plan were applied. */
enum class ExecutionReadiness {
    Ready,                    ///< every intent is honoured
    Constrained,              ///< honoured, but bounded by safety (thermal, stale telemetry)
    CapabilityLimited,        ///< the device cannot honour part of the ask; generic survives
    Unsupported,              ///< nothing in the ask can be honoured here
    DeviceValidationRequired, ///< it could be, but nobody has confirmed it on this hardware
    InvalidPlan,              ///< the plan failed validation; nothing may be applied
    TelemetryDegraded,        ///< the data behind the decision is stale or offline
    RestorationUnavailable,   ///< a restore was asked for and the original cannot be given back
};

const char *execution_readiness_name(ExecutionReadiness readiness);

/**
 * @brief One action the live engine would perform. Fully specified, never executed.
 *
 * Deliberately shaped like what ExecutionEngine::apply already consumes, so the live cutover
 * wires this in rather than rewriting planning a second time.
 */
struct DryRunAction {
    std::string action_id;
    std::string capability_id;
    std::string descriptor_id;
    std::string descriptor_set;

    std::string backend;      ///< "sysfs" today; zen will be "android_zen"
    std::string target_path;  ///< validated and allowlist-checked; recorded, never opened here
    std::string desired_value;
    NodeValueType value_type = NodeValueType::Token;

    ReadBackStrategy read_back = ReadBackStrategy::Exact;
    RollbackStrategy rollback = RollbackStrategy::RestoreOriginal;
    bool rollback_value_available = false;

    int order_group = 0;
    std::vector<std::string> depends_on;
    std::string conflict_group;
    bool critical = false;

    std::string source_intent_id;
    flux::engine::DecisionReason reason = flux::engine::DecisionReason::startup;
    CapabilityState capability_state = CapabilityState::Supported;
    ValidationStatus validation = ValidationStatus::PhysicalDeviceRequired;
};

/** An action that would NOT happen, and precisely why. */
struct PreventedAction {
    std::string capability_id;
    std::string descriptor_id;
    std::string descriptor_set;
    CapabilityState state = CapabilityState::Unsupported;
    CapabilityReason reason = CapabilityReason::NotProbed;
    bool was_critical = false;
    std::string detail;
};

/** What the runtime would look like afterwards. A projection, never a claim. */
struct ProjectedRuntimeProfileState {
    flux::engine::TargetProfile requested_profile = flux::engine::TargetProfile::Balanced;
    std::string requested_intent;
    std::string effective_intent; ///< what would actually be achieved

    /// Carried through untouched. A dry run must never advance this: nothing was applied, so
    /// the last verified live profile is exactly what it was.
    flux::engine::TargetProfile last_verified_profile = flux::engine::TargetProfile::Balanced;

    ExecutionReadiness readiness = ExecutionReadiness::Ready;
    flux::engine::DataHealth telemetry_health = flux::engine::DataHealth::Offline;

    bool would_be_fully_active = false; ///< every critical intent honoured
    bool device_validation_pending = false;
    bool rollback_fully_covered = false;

    std::string degraded_reason;
    std::string unsupported_reason;
};

/** An immutable, inspectable specification of what would happen. */
struct DryRunExecutionPlan {
    PolicyIntent requested;
    std::string effective_intent_id;

    std::vector<DryRunAction> actions;      ///< would execute, in order
    std::vector<PreventedAction> prevented; ///< would not, with reasons
    std::vector<ResolutionStep> trace;

    bool valid = false;
    std::string invalid_reason;
    bool critical_rejection = false;

    int optional_skips = 0;
    int rollback_covered = 0;

    ProjectedRuntimeProfileState projected;

    [[nodiscard]] size_t candidate_count() const { return actions.size() + prevented.size(); }
    [[nodiscard]] size_t executable_count() const { return actions.size(); }
    [[nodiscard]] size_t prevented_count() const { return prevented.size(); }
};

/**
 * @brief Resolves intents against device packs and probe results.
 *
 * Takes the probe by reference; the probe reads, and this class does not even do that. The
 * separation is deliberate: everything here is a pure function of the probe's answers, so the
 * planning logic is testable with fabricated CapabilityReports and no filesystem at all.
 */
class DryRunPlanner {
public:
    explicit DryRunPlanner(const CapabilityProbe &probe) : probe_(probe) {}

    /**
     * @brief Compile a plan for @p intents against @p packs on @p identity.
     *
     * @param last_verified the caller's current verified profile, carried into the projection
     *                      untouched. A dry run cannot change it.
     */
    [[nodiscard]] DryRunExecutionPlan plan(const CapabilityIntentSet &intents,
                                           const std::vector<DevicePack> &packs,
                                           const DeviceIdentity &identity,
                                           flux::engine::TargetProfile last_verified,
                                           int64_t now_ms) const;

private:
    const CapabilityProbe &probe_;
};

} // namespace flux::execution
