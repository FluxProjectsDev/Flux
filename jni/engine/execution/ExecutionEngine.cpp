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

#include "ExecutionEngine.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <unordered_set>

namespace flux::execution {

namespace {

/// Local, because the decision engine has no reason to grow a formatting helper for the
/// execution engine's history buffer.
const char *health_name(flux::engine::DataHealth health) {
    switch (health) {
        case flux::engine::DataHealth::Healthy: return "healthy";
        case flux::engine::DataHealth::Stale: return "stale";
        case flux::engine::DataHealth::Offline: return "offline";
    }
    return "unknown";
}

} // namespace

const char *plan_rejection_name(PlanRejection rejection) {
    switch (rejection) {
        case PlanRejection::None: return "none";
        case PlanRejection::NotExecutable: return "not_executable";
        case PlanRejection::CapabilityNotSupported: return "capability_not_supported";
        case PlanRejection::UnsafePath: return "unsafe_path";
        case PlanRejection::ConflictingTargets: return "conflicting_targets";
        case PlanRejection::InvalidValue: return "invalid_value";
        case PlanRejection::MissingDependency: return "missing_dependency";
        case PlanRejection::DependencyCycle: return "dependency_cycle";
        case PlanRejection::UnsupportedVerification: return "unsupported_verification";
        case PlanRejection::RollbackImpossible: return "rollback_impossible";
        case PlanRejection::CapabilityGenerationChanged: return "capability_generation_changed";
    }
    return "unknown";
}

// --- LivePlanCompiler ------------------------------------------------------

namespace {

/// Reject a rejected plan uniformly: an invalid plan carries no actions, ever. A plan that
/// keeps its actions alongside a rejection is one accidental `if` away from executing them.
ExecutionPlan reject(ExecutionPlan plan, PlanRejection rejection, std::string reason) {
    plan.valid = false;
    plan.rejection = rejection;
    plan.invalid_reason = std::move(reason);
    plan.actions.clear();
    return plan;
}

bool value_is_valid(const ExecutionAction &action) {
    if (action.desired_value.empty()) return false;
    if (action.desired_value.find('\0') != std::string::npos) return false;
    if (action.value_type == NodeValueType::Integer) {
        errno = 0;
        char *end = nullptr;
        (void)std::strtol(action.desired_value.c_str(), &end, 10);
        if (errno != 0 || end != action.desired_value.c_str() + action.desired_value.size()) {
            return false;
        }
    }
    return true;
}

} // namespace

ExecutionPlan LivePlanCompiler::compile(const DryRunExecutionPlan &dry_run,
                                        const NodeBackend &backend, const PathPolicy &policy,
                                        uint64_t capability_generation) {
    ExecutionPlan plan;
    plan.plan_id = dry_run.requested.intent_id + "@" + std::to_string(capability_generation);
    plan.capability_generation = capability_generation;
    plan.requested_intent = dry_run.requested;
    plan.effective_intent_id = dry_run.effective_intent_id;
    plan.requested_profile = dry_run.requested.source_profile;
    plan.prevented = dry_run.prevented;
    plan.skipped_unsupported = dry_run.optional_skips;
    plan.critical_rejection = dry_run.critical_rejection;
    plan.readiness = dry_run.projected.readiness;

    if (!dry_run.valid) {
        return reject(std::move(plan), PlanRejection::NotExecutable,
                      "the dry run produced no valid plan: " + dry_run.invalid_reason);
    }

    // A plan with nothing to do is valid and does nothing. That is the honest outcome on a
    // device with no supported capability, and it must not be confused with a failure.
    if (dry_run.actions.empty()) {
        plan.valid = true;
        return plan;
    }

    std::unordered_map<std::string, std::string> target_values; // path -> desired
    std::unordered_set<std::string> action_ids;

    for (const auto &source : dry_run.actions) {
        // The gate, restated at compile time. The dry run already filtered on this, but the
        // whole point of the gate is that nothing executable exists without it, so the thing
        // that produces executable actions checks it itself rather than trusting its input.
        if (!capability_is_executable(source.capability_state)) {
            return reject(std::move(plan), PlanRejection::CapabilityNotSupported,
                          "capability '" + source.capability_id + "' is " +
                              capability_state_name(source.capability_state) +
                              ", which may never produce a write");
        }

        ExecutionAction action;
        action.action_id = source.action_id;
        action.capability_id = source.capability_id;
        action.descriptor_id = source.descriptor_id;
        action.descriptor_set = source.descriptor_set;
        action.path = source.target_path;
        action.desired_value = source.desired_value;
        action.value_type = source.value_type;
        action.read_back = source.read_back;
        action.rollback = source.rollback;
        action.order_group = source.order_group;
        action.conflict_group = source.conflict_group;
        action.depends_on = source.depends_on;
        action.critical = source.critical;
        action.reason = flux::engine::decision_reason_name(source.reason);
        action.source_intent_id = source.source_intent_id;
        action.rollback_required = source.rollback == RollbackStrategy::RestoreOriginal;

        if (policy.check(action.path) != NodeError::Ok) {
            return reject(std::move(plan), PlanRejection::UnsafePath,
                          "action '" + action.action_id + "' targets a path outside the approved roots");
        }
        if (!value_is_valid(action)) {
            return reject(std::move(plan), PlanRejection::InvalidValue,
                          "action '" + action.action_id + "' carries an invalid desired value");
        }
        if (!action_ids.insert(action.action_id).second) {
            return reject(std::move(plan), PlanRejection::ConflictingTargets,
                          "duplicate action id '" + action.action_id + "'");
        }

        // Two actions writing different values to one node is a plan that cannot be satisfied.
        // Whichever ran last would win, so the plan's outcome would depend on its order rather
        // than its content.
        auto existing = target_values.find(action.path);
        if (existing != target_values.end() && existing->second != action.desired_value) {
            return reject(std::move(plan), PlanRejection::ConflictingTargets,
                          "two actions want different values on one node");
        }
        target_values[action.path] = action.desired_value;

        // Capture the original now, so a required rollback is known to be possible *before*
        // anything is written rather than discovered to be impossible afterwards.
        action.previous_value = backend.read(action.path);
        if (action.rollback_required && !action.previous_value) {
            return reject(std::move(plan), PlanRejection::RollbackImpossible,
                          "action '" + action.action_id +
                              "' requires rollback but its original value cannot be read");
        }

        plan.actions.push_back(std::move(action));
    }

    // Dependencies must exist inside the plan. A dependency on something that was prevented is
    // a missing dependency: the action would run against a precondition that never happened.
    for (const auto &action : plan.actions) {
        for (const auto &dependency : action.depends_on) {
            const bool present = std::any_of(
                plan.actions.begin(), plan.actions.end(),
                [&](const ExecutionAction &other) { return other.descriptor_id == dependency; });
            if (!present) {
                return reject(std::move(plan), PlanRejection::MissingDependency,
                              "action '" + action.action_id + "' depends on '" + dependency +
                                  "', which is not in this plan");
            }
            if (dependency == action.descriptor_id) {
                return reject(std::move(plan), PlanRejection::DependencyCycle,
                              "action '" + action.action_id + "' depends on itself");
            }
        }
    }

    // Deterministic order: (order_group, action_id). Compiled, not re-derived — the dry run
    // already sorted this way, and re-sorting to a different rule would mean the plan that ran
    // was not the plan that was inspected.
    std::stable_sort(plan.actions.begin(), plan.actions.end(),
                     [](const ExecutionAction &a, const ExecutionAction &b) {
                         if (a.order_group != b.order_group) return a.order_group < b.order_group;
                         return a.action_id < b.action_id;
                     });

    plan.valid = true;
    return plan;
}

// --- ExecutionEngine -------------------------------------------------------

bool ExecutionEngine::capture_original(const std::string &id, const std::string &path) {
    if (originals_.count(id)) return true; // first-seen only; never overwrite the true original
    auto current = backend_.read(path);
    if (!current) return false;
    originals_[id] = Original{path, *current};
    return true;
}

std::optional<std::string> ExecutionEngine::verified_value(const std::string &id) const {
    auto it = verified_.find(id);
    if (it == verified_.end()) return std::nullopt;
    return it->second;
}

bool ExecutionEngine::value_holds(const ExecutionAction &action) const {
    const auto observed = backend_.read(action.path);
    if (!observed) return false;

    switch (action.read_back) {
        case ReadBackStrategy::None:
            // The node cannot be read back meaningfully. Nothing is claimed about it: the write
            // is reported as performed, never as verified.
            return true;
        case ReadBackStrategy::Exact:
            return *observed == action.desired_value;
        case ReadBackStrategy::Contains:
            return observed->find(action.desired_value) != std::string::npos;
        case ReadBackStrategy::Numeric: {
            errno = 0;
            char *lhs_end = nullptr;
            char *rhs_end = nullptr;
            const long lhs = std::strtol(observed->c_str(), &lhs_end, 10);
            const long rhs = std::strtol(action.desired_value.c_str(), &rhs_end, 10);
            if (errno != 0 || lhs_end == observed->c_str() ||
                rhs_end == action.desired_value.c_str()) {
                return false;
            }
            return lhs == rhs;
        }
    }
    return false;
}

std::vector<std::string> ExecutionEngine::detect_external_mutation() {
    std::vector<std::string> drifted;
    for (const auto &[id, expected] : verified_) {
        auto path = verified_paths_.find(id);
        if (path == verified_paths_.end()) continue;
        const auto observed = backend_.read(path->second);
        if (!observed || *observed != expected) drifted.push_back(id);
    }
    // Forget what the device no longer agrees with, so the next apply rewrites it instead of
    // skipping it as already done. A cached "verified" value that the device has since lost is
    // worse than no cache: it makes Flux confidently wrong.
    for (const auto &id : drifted) verified_.erase(id);
    std::sort(drifted.begin(), drifted.end());
    return drifted;
}

ApplyResult ExecutionEngine::apply(const ExecutionPlan &plan, const std::string &previous_profile,
                                   uint64_t live_capability_generation, int64_t now_ms) {
    ApplyResult r;
    r.plan_id = plan.plan_id;
    r.requested_profile = flux::engine::target_profile_name(plan.requested_profile);
    r.previous_profile = previous_profile;
    r.reason = flux::engine::decision_reason_name(plan.requested_intent.reason);
    r.timestamp_ms = now_ms;
    r.skipped_unsupported = plan.skipped_unsupported;
    r.prevented_count = static_cast<int>(plan.prevented.size());
    r.action_count = static_cast<int>(plan.actions.size());

    if (!plan.valid) {
        r.plan_rejected = true;
        r.rejection = plan.rejection;
        r.critical_failure = plan.critical_rejection;
        r.verified_active = false;
        r.message = std::string("plan rejected (") + plan_rejection_name(plan.rejection) +
                    "): " + plan.invalid_reason;
        record_history(r, plan);
        return r;
    }

    // Time-of-check/time-of-use. The plan describes a device that was probed some time ago; if
    // the capability generation has moved, something about the device changed and every
    // conclusion in the plan is suspect. Refuse and let the caller re-plan — do not write a
    // stale plan and find out afterwards.
    if (plan.capability_generation != live_capability_generation) {
        r.plan_rejected = true;
        r.rejection = PlanRejection::CapabilityGenerationChanged;
        r.verified_active = false;
        r.message = "capability generation changed between planning and applying";
        record_history(r, plan);
        return r;
    }

    if (plan.actions.empty()) {
        // A conservative no-op. Nothing was executable, so nothing was written and nothing is
        // claimed: verified_active stays false precisely because no critical action proved
        // anything.
        r.verified_active = false;
        r.message = "no executable action for this intent on this device";
        record_history(r, plan);
        return r;
    }

    // --- preflight -------------------------------------------------------
    // The whole critical group is validated before any critical write happens. Discovering a
    // broken precondition halfway through leaves the device in a combination nobody designed.
    for (const auto &action : plan.actions) {
        if (!action.critical) continue;
        if (!backend_.exists(action.path)) {
            r.plan_rejected = true;
            r.rejection = PlanRejection::CapabilityNotSupported;
            r.critical_failure = true;
            r.degraded_capability = action.capability_id;
            r.worst_error = NodeError::NotFound;
            r.message = "critical capability vanished before apply: " + action.capability_id;
            record_history(r, plan);
            return r;
        }
        if (action.rollback_required && !capture_original(action.capability_id, action.path)) {
            r.plan_rejected = true;
            r.rejection = PlanRejection::RollbackImpossible;
            r.rollback_unavailable = true;
            r.critical_failure = true;
            r.degraded_capability = action.capability_id;
            r.message = "cannot capture the original value of a critical capability that "
                        "requires rollback: " +
                        action.capability_id;
            record_history(r, plan);
            return r;
        }
    }

    struct Undo {
        std::string id;
        std::string path;
        std::optional<std::string> previous;
    };
    std::vector<Undo> critical_undo;

    for (const auto &action : plan.actions) {
        // Idempotency: a value already verified in place is not rewritten. This is what makes a
        // repeated decision free rather than a storm of identical writes.
        auto verified = verified_.find(action.capability_id);
        if (verified != verified_.end() && verified->second == action.desired_value) {
            ++r.skipped_idempotent;
            continue;
        }

        const bool have_original = capture_original(action.capability_id, action.path);
        if (action.rollback_required && !have_original) {
            // Non-critical and non-restorable: skip rather than perform a write that could
            // never be undone. An optional tweak is not worth a permanent change.
            ++r.optional_failures;
            r.rollback_unavailable = true;
            continue;
        }

        const NodeWriteResult write = backend_.write_checked(action.path, action.desired_value);

        if (!write.ok()) {
            if (write.error != NodeError::Ok) r.worst_error = write.error;
            if (action.critical) {
                r.critical_failure = true;
                r.degraded_capability = action.capability_id;
                r.message = std::string("critical write failed (") + node_error_name(write.error) +
                            "): " + action.capability_id;
                break;
            }
            ++r.optional_failures;
            continue;
        }

        if (action.critical) critical_undo.push_back(Undo{action.capability_id, action.path,
                                                          action.previous_value});

        // Verify before believing. A write(2) that returned success proves the kernel accepted
        // the bytes, not that the node took the value: sysfs nodes routinely clamp, round or
        // ignore what they are given.
        if (!value_holds(action)) {
            if (action.critical) {
                r.critical_failure = true;
                r.worst_error = NodeError::VerifyMismatch;
                r.degraded_capability = action.capability_id;
                r.message = "critical verify mismatch: " + action.capability_id;
                break;
            }
            ++r.optional_failures;
            r.worst_error = NodeError::VerifyMismatch;
            continue;
        }

        if (action.read_back != ReadBackStrategy::None) {
            verified_[action.capability_id] = action.desired_value;
            verified_paths_[action.capability_id] = action.path;
        }
        ++r.succeeded;
    }

    if (r.critical_failure) {
        r.rollback_attempted = true;
        r.rollback_succeeded = true;
        // Reverse order: undo the most recent change first, so a dependency is never left
        // pointing at a value its dependent has already given up.
        for (auto it = critical_undo.rbegin(); it != critical_undo.rend(); ++it) {
            verified_.erase(it->id);
            if (!it->previous) {
                r.rollback_succeeded = false;
                r.rollback_unavailable = true;
                r.degraded = true;
                r.degraded_capability = it->id;
                continue;
            }
            const auto undo = backend_.write_checked(it->path, *it->previous);
            if (undo.ok()) {
                verified_[it->id] = *it->previous;
                verified_paths_[it->id] = it->path;
            } else {
                r.rollback_succeeded = false;
                r.degraded = true;
                r.degraded_capability = it->id;
            }
        }
        r.verified_active = false;
        if (r.message.empty()) r.message = "critical failure; rollback attempted";
    } else {
        // Every critical action was written and verified. This is the only path that may report
        // the plan as active — and even here, "active" means what was executable was verified,
        // not that the device got everything the user asked for.
        r.verified_active = true;
        r.message = "applied " + std::to_string(r.succeeded) + " action(s), " +
                    std::to_string(r.skipped_idempotent) + " idempotent, " +
                    std::to_string(r.optional_failures) + " optional failure(s), " +
                    std::to_string(r.prevented_count) + " prevented";
    }

    record_history(r, plan);
    return r;
}

ApplyResult ExecutionEngine::restore_originals(const std::string &reason, int64_t now_ms) {
    ApplyResult r;
    r.requested_profile = "restore";
    r.reason = reason;
    r.timestamp_ms = now_ms;
    r.action_count = static_cast<int>(originals_.size());
    r.verified_active = true;

    // Deterministic order, so a restore is reproducible and diffable like any other apply.
    std::vector<std::string> ids;
    ids.reserve(originals_.size());
    for (const auto &[id, unused] : originals_) ids.push_back(id);
    std::sort(ids.begin(), ids.end());

    for (const auto &id : ids) {
        const auto &original = originals_[id];
        const auto write = backend_.write_checked(original.path, original.value);
        if (write.ok()) {
            verified_[id] = original.value;
            verified_paths_[id] = original.path;
            ++r.succeeded;
        } else {
            ++r.optional_failures;
            r.verified_active = false;
            r.degraded = true;
            r.degraded_capability = id;
            r.worst_error = write.error;
            verified_.erase(id);
        }
    }
    r.message = "restored " + std::to_string(r.succeeded) + " original value(s)";
    ExecutionPlan empty_plan;
    record_history(r, empty_plan);
    return r;
}

void ExecutionEngine::record_history(const ApplyResult &result, const ExecutionPlan &plan) {
    ApplyHistoryEntry e;
    e.monotonic_ms = result.timestamp_ms;
    e.plan_id = result.plan_id;
    e.previous_verified_profile = result.previous_profile;
    e.requested_profile = result.requested_profile;
    e.effective_profile = plan.effective_intent_id;
    e.reason = result.reason;
    e.priority = static_cast<int>(plan.requested_intent.priority);
    e.telemetry_health = health_name(plan.requested_intent.health);
    e.capability_health = execution_readiness_name(plan.readiness);
    e.action_count = result.action_count;
    e.succeeded = result.succeeded;
    e.prevented_count = result.prevented_count;
    e.optional_failures = result.optional_failures;
    e.critical_failure = result.critical_failure;
    e.rollback_attempted = result.rollback_attempted;
    e.rollback_succeeded = result.rollback_succeeded;
    e.verified_active = result.verified_active;
    e.degraded = result.degraded;

    // Sanitized: a category and, at most, a capability id. Never a path, never an errno string.
    // This buffer is destined for a diagnostics export a user can hand to a stranger.
    if (result.plan_rejected) {
        e.error_category = plan_rejection_name(result.rejection);
    } else if (result.worst_error != NodeError::Ok) {
        e.error_category = node_error_name(result.worst_error);
    }

    history_.push_back(std::move(e));
    while (history_.size() > history_capacity_) history_.pop_front();
}

} // namespace flux::execution
