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
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "DryRunPlanner.hpp"
#include "NodeBackend.hpp"

/**
 * @file ExecutionEngine.hpp
 * @brief Flux V2 Execution Engine — capability-aware, validated, verified, and
 *        rollback-aware application of a desired profile.
 *
 * Independent Flux implementation written from the Flux decision-output semantics,
 * documented Linux sysfs behaviour, detected device capabilities, and explicit
 * safety/rollback requirements. It is not a translation of the legacy applier.
 *
 * The engine performs writes through an injectable NodeBackend, so all policy and
 * transactional logic is host-testable with an in-memory backend — no root, no
 * sysfs, no device. It selects no profiles, parses no telemetry, and detects no
 * foreground apps: it only applies a plan someone else decided on.
 */
namespace flux::execution {

// --- Plan ------------------------------------------------------------------
//
// There is exactly one planner in Flux: intents resolve against probed descriptors in
// DryRunPlanner, and the result is *compiled* here into the live plan. This file used to carry
// a second, independent model — CapabilityRegistry/NodeDescriptor/ExecutionPlanner, which
// planned from a ProfilePlanSpec against a path-exists check. Keeping both would have meant two
// answers to "can this device do this", and the one that wrote would not have been the one the
// tests, the dry run and the UI had inspected. It was never wired to the daemon; the descriptor
// model replaces it.

/** One fully specified, validated write. Compiled from a DryRunAction, never authored here. */
struct ExecutionAction {
    std::string action_id;
    std::string capability_id;
    std::string descriptor_id;
    std::string descriptor_set;

    std::string path;
    std::string desired_value;
    NodeValueType value_type = NodeValueType::Token;

    /// The value to put back. nullopt means the original could not be read, which makes any
    /// rollback of this action impossible — the engine must not invent one.
    std::optional<std::string> previous_value;
    bool rollback_required = false;

    ReadBackStrategy read_back = ReadBackStrategy::Exact;
    RollbackStrategy rollback = RollbackStrategy::RestoreOriginal;

    int order_group = 0;
    std::string conflict_group;
    std::string dependency_group;
    std::vector<std::string> depends_on;
    bool critical = false;

    std::string reason;
    std::string source_intent_id;
};

/** Why a compiled plan may not be executed. */
enum class PlanRejection {
    None,
    NotExecutable,          ///< the dry run itself produced no valid plan
    CapabilityNotSupported, ///< an action's capability is not Supported
    UnsafePath,
    ConflictingTargets,     ///< two actions want different values on one node
    InvalidValue,
    MissingDependency,
    DependencyCycle,
    UnsupportedVerification,
    RollbackImpossible,     ///< rollback is required and the original is unknown
    CapabilityGenerationChanged, ///< the device changed between planning and applying
};

const char *plan_rejection_name(PlanRejection rejection);

/**
 * @brief An immutable, fully-validated, executable plan.
 *
 * Compiled from a DryRunExecutionPlan. It carries the requested and the effective intent
 * separately: what was asked for is not what the device could do, and collapsing the two is how
 * a constrained apply gets reported as a full one.
 */
struct ExecutionPlan {
    std::string plan_id;
    uint64_t capability_generation = 0; ///< the device state this plan was compiled against

    PolicyIntent requested_intent;
    std::string effective_intent_id;
    flux::engine::TargetProfile requested_profile = flux::engine::TargetProfile::Balanced;

    std::vector<ExecutionAction> actions;
    std::vector<PreventedAction> prevented;

    int skipped_unsupported = 0;
    bool critical_rejection = false;
    ExecutionReadiness readiness = ExecutionReadiness::Unsupported;

    bool valid = false;
    PlanRejection rejection = PlanRejection::None;
    std::string invalid_reason;

    [[nodiscard]] bool empty() const { return actions.empty(); }
    [[nodiscard]] size_t action_count() const { return actions.size(); }
};

/**
 * @brief Compiles a validated dry-run plan into an executable one. Adds no planning decisions.
 *
 * The compiler re-validates rather than trusting: a plan is a projection of a device state that
 * may have changed since. It refuses, it never repairs — a plan that cannot be executed exactly
 * as inspected is not silently reduced to one that can.
 */
class LivePlanCompiler {
public:
    /**
     * @brief Compile @p dry_run into an executable plan.
     *
     * @param policy the same approved roots the backend and the probe used. Injected, never
     *        assumed: a compiler that re-checked against its own idea of the roots would
     *        approve targets the backend then refuses, or worse, the reverse.
     * @param capability_generation the generation the dry run was planned against; the engine
     *        re-checks it at apply time to catch a device that changed underneath the plan.
     */
    [[nodiscard]] static ExecutionPlan compile(const DryRunExecutionPlan &dry_run,
                                               const NodeBackend &backend, const PathPolicy &policy,
                                               uint64_t capability_generation);
};

// --- Apply -----------------------------------------------------------------

struct ApplyResult {
    std::string plan_id;
    std::string requested_profile;
    std::string previous_profile;
    std::string reason;

    int action_count = 0;
    int succeeded = 0;
    int skipped_unsupported = 0;
    int skipped_idempotent = 0;
    int optional_failures = 0;
    int prevented_count = 0;
    bool critical_failure = false;

    bool plan_rejected = false;
    PlanRejection rejection = PlanRejection::None;

    bool rollback_attempted = false;
    bool rollback_succeeded = false;
    bool rollback_unavailable = false; ///< a rollback was needed and no original was known

    /// Every critical action verified to hold its desired value. This — and only this — is what
    /// lets a caller advance the verified profile.
    bool verified_active = false;
    bool degraded = false; ///< rollback failed: the runtime is in a state nobody designed
    std::string degraded_capability;

    /// The worst error category seen, sanitized: a category and a capability id, never a path
    /// or an errno string. History is destined for a diagnostics channel a user can export.
    NodeError worst_error = NodeError::Ok;

    int64_t timestamp_ms = 0;
    std::string message;
};

/**
 * @brief One bounded history record.
 *
 * Everything a user or a maintainer needs to answer "why is my device in this state" without
 * reproducing it. Sized and bounded: this is a ring buffer in memory, not a log on disk.
 */
struct ApplyHistoryEntry {
    int64_t monotonic_ms = 0;
    uint64_t telemetry_sequence = 0;
    std::string plan_id;

    std::string previous_verified_profile;
    std::string requested_profile;
    std::string effective_profile;
    std::string reason;
    int priority = 0;
    std::string telemetry_health;
    std::string capability_health;

    int action_count = 0;
    int succeeded = 0;
    int prevented_count = 0;
    int optional_failures = 0;
    bool critical_failure = false;

    bool rollback_attempted = false;
    bool rollback_succeeded = false;

    bool verified_active = false;
    bool degraded = false;

    /// A category, never a raw errno or a device path.
    std::string error_category;
};

/**
 * @brief Applies compiled plans transactionally, with verification, rollback and idempotency.
 *
 * The engine is the only thing in Flux that writes a device node, and it writes only what a
 * plan told it to. It selects no profiles, reads no telemetry, and knows about no SoC.
 */
class ExecutionEngine {
public:
    explicit ExecutionEngine(NodeBackend &backend, size_t history_capacity = 64)
        : backend_(backend), history_capacity_(history_capacity) {}

    /**
     * @brief Apply @p plan, moving from @p previous_profile to the plan's requested profile.
     *
     * Sequence per critical group: re-validate the plan against the live capability generation
     * -> capture originals -> compute the diff from verified state -> apply in order -> read
     * back and verify -> roll the group back on critical failure -> publish a complete
     * ApplyResult. No critical write happens until the whole group has passed preflight.
     *
     * The caller advances the verified profile only when verified_active is true.
     */
    ApplyResult apply(const ExecutionPlan &plan, const std::string &previous_profile,
                      uint64_t live_capability_generation, int64_t now_ms);

    /** Restore every first-seen original value (session end, shutdown, etc.). */
    ApplyResult restore_originals(const std::string &reason, int64_t now_ms);

    /// Idempotency: forget verified values so the next apply rewrites (daemon restart,
    /// external mutation, capability invalidation, config change).
    void invalidate_all() { verified_.clear(); }
    void invalidate(const std::string &capability_id) { verified_.erase(capability_id); }

    /// Re-read every capability whose value Flux believes it verified, and forget the ones the
    /// device no longer agrees with. This is how external mutation stops being invisible.
    /// @return the capability ids that had drifted.
    std::vector<std::string> detect_external_mutation();

    [[nodiscard]] const std::deque<ApplyHistoryEntry> &history() const { return history_; }
    [[nodiscard]] std::optional<std::string> verified_value(const std::string &id) const;
    [[nodiscard]] size_t tracked_originals() const { return originals_.size(); }

private:
    NodeBackend &backend_;
    size_t history_capacity_;

    struct Original {
        std::string path;
        std::string value;
    };

    std::unordered_map<std::string, std::string> verified_; ///< last verified value per capability
    std::unordered_map<std::string, Original> originals_;    ///< first-seen value per capability
    std::unordered_map<std::string, std::string> verified_paths_;

    std::deque<ApplyHistoryEntry> history_;

    void record_history(const ApplyResult &result, const ExecutionPlan &plan);
    bool capture_original(const std::string &id, const std::string &path);
    [[nodiscard]] bool value_holds(const ExecutionAction &action) const;
};

} // namespace flux::execution
