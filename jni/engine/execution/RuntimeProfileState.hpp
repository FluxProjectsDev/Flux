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

#include "ExecutionEngine.hpp"

/**
 * @file RuntimeProfileState.hpp
 * @brief The one canonical answer to "what profile is this device actually in".
 *
 * Flux-owned (Category A).
 *
 * ## Why this exists
 *
 * The legacy daemon had no such thing. It ran a shell function and, if the function returned,
 * recorded the profile as current — so "current" meant "we asked for it", not "the device is in
 * it". A node that refused the write, a vendor path that did not exist, a value the kernel
 * clamped: all of them produced a device in one state and a daemon convinced of another.
 *
 * This store separates the four things that were conflated:
 *
 *  - **requested**: what policy asked for.
 *  - **effective**: what the device could actually be asked for, after capability limits.
 *  - **verified**: what was written *and read back*. The only honest answer to "what is active".
 *  - **apply state**: where in the cycle the runtime is right now.
 *
 * Nothing advances `verified` except a successful, verified apply. A Decision does not. A plan
 * does not. This is the single rule the whole increment exists to enforce.
 */
namespace flux::execution {

/** Where the runtime is, and how much of the ask it managed to deliver. */
enum class ApplyState {
    Idle,                   ///< nothing in flight
    Planning,               ///< building and compiling a plan
    Applying,               ///< writes in progress
    Verified,               ///< every critical action written and read back
    Constrained,            ///< applied, but bounded by safety (thermal, stale telemetry)
    CapabilityLimited,      ///< applied what the device supports; the ask was not fully met
    Unsupported,            ///< nothing here can serve the ask; nothing was written
    Degraded,               ///< a rollback failed: the device is in a state nobody designed
    RollbackInProgress,
    RollbackFailed,
    ExternalMutation,       ///< something outside Flux moved a node Flux had verified
    RestorationUnavailable, ///< a restore was required and the original cannot be given back
};

const char *apply_state_name(ApplyState state);

/// True for states where Flux believes the device holds what it last verified.
[[nodiscard]] bool apply_state_is_settled(ApplyState state);

/**
 * @brief The canonical runtime state. One instance, owned by the composition root.
 */
class RuntimeProfileState {
public:
    /// Called when planning begins. Records the ask; changes nothing about what is verified.
    void begin_planning(flux::engine::TargetProfile requested, const std::string &requested_intent,
                        flux::engine::DataHealth telemetry_health, int64_t now_ms);

    /// Called with the compiled plan, before it runs. Still changes nothing about `verified`:
    /// a plan is a projection, and a projection is not a device.
    void begin_applying(const ExecutionPlan &plan, int64_t now_ms);

    /**
     * @brief Record the outcome of an apply. The only thing that may advance `verified`.
     *
     * @param result the engine's complete report.
     * @param plan the plan that produced it, for the effective-intent and readiness context.
     */
    void record_apply(const ApplyResult &result, const ExecutionPlan &plan, int64_t now_ms);

    /// Something outside Flux moved a node Flux had verified. The verified profile is no longer
    /// a claim Flux can make, whatever it last wrote.
    void record_external_mutation(int64_t now_ms);

    void record_restore(const ApplyResult &result, int64_t now_ms);

    [[nodiscard]] flux::engine::TargetProfile requested_profile() const { return requested_; }
    [[nodiscard]] flux::engine::TargetProfile effective_profile() const { return effective_; }

    /// The last profile that was written and read back. Not what was asked for, and not what a
    /// plan said would happen.
    [[nodiscard]] flux::engine::TargetProfile verified_profile() const { return verified_; }

    [[nodiscard]] bool has_verified_profile() const { return has_verified_; }
    [[nodiscard]] ApplyState state() const { return state_; }
    [[nodiscard]] const std::string &requested_intent() const { return requested_intent_; }
    [[nodiscard]] const std::string &effective_intent() const { return effective_intent_; }
    [[nodiscard]] flux::engine::DataHealth telemetry_health() const { return telemetry_health_; }
    [[nodiscard]] ExecutionReadiness capability_health() const { return capability_health_; }
    [[nodiscard]] const std::string &degraded_reason() const { return degraded_reason_; }
    [[nodiscard]] int64_t last_verified_apply_ms() const { return last_verified_apply_ms_; }
    [[nodiscard]] bool device_validation_pending() const { return device_validation_pending_; }
    [[nodiscard]] bool rollback_failed() const { return rollback_failed_; }

    /// True only when the device is verified to hold everything the ask implied. A constrained
    /// or capability-limited apply is a success, but it is not this.
    [[nodiscard]] bool fully_optimized() const { return fully_optimized_; }

private:
    flux::engine::TargetProfile requested_ = flux::engine::TargetProfile::Balanced;
    flux::engine::TargetProfile effective_ = flux::engine::TargetProfile::Balanced;
    flux::engine::TargetProfile verified_ = flux::engine::TargetProfile::Balanced;
    bool has_verified_ = false;

    ApplyState state_ = ApplyState::Idle;
    std::string requested_intent_;
    std::string effective_intent_;
    flux::engine::DataHealth telemetry_health_ = flux::engine::DataHealth::Offline;
    ExecutionReadiness capability_health_ = ExecutionReadiness::Unsupported;
    std::string degraded_reason_;
    int64_t last_verified_apply_ms_ = 0;
    bool device_validation_pending_ = false;
    bool rollback_failed_ = false;
    bool fully_optimized_ = false;
};

} // namespace flux::execution
