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

#include "RuntimeProfileState.hpp"

namespace flux::execution {

const char *apply_state_name(ApplyState state) {
    switch (state) {
        case ApplyState::Idle: return "idle";
        case ApplyState::Planning: return "planning";
        case ApplyState::Applying: return "applying";
        case ApplyState::Verified: return "verified";
        case ApplyState::Constrained: return "constrained";
        case ApplyState::CapabilityLimited: return "capability_limited";
        case ApplyState::Unsupported: return "unsupported";
        case ApplyState::Degraded: return "degraded";
        case ApplyState::RollbackInProgress: return "rollback_in_progress";
        case ApplyState::RollbackFailed: return "rollback_failed";
        case ApplyState::ExternalMutation: return "external_mutation";
        case ApplyState::RestorationUnavailable: return "restoration_unavailable";
    }
    return "unknown";
}

bool apply_state_is_settled(ApplyState state) {
    switch (state) {
        case ApplyState::Verified:
        case ApplyState::Constrained:
        case ApplyState::CapabilityLimited:
            return true;
        case ApplyState::Idle:
        case ApplyState::Planning:
        case ApplyState::Applying:
        case ApplyState::Unsupported:
        case ApplyState::Degraded:
        case ApplyState::RollbackInProgress:
        case ApplyState::RollbackFailed:
        case ApplyState::ExternalMutation:
        case ApplyState::RestorationUnavailable:
            return false;
    }
    return false;
}

void RuntimeProfileState::begin_planning(flux::engine::TargetProfile requested,
                                         const std::string &requested_intent,
                                         flux::engine::DataHealth telemetry_health,
                                         int64_t /*now_ms*/) {
    requested_ = requested;
    requested_intent_ = requested_intent;
    telemetry_health_ = telemetry_health;
    state_ = ApplyState::Planning;
    // Deliberately absent: any change to verified_. Asking is not achieving.
}

void RuntimeProfileState::begin_applying(const ExecutionPlan &plan, int64_t /*now_ms*/) {
    effective_intent_ = plan.effective_intent_id;
    capability_health_ = plan.readiness;
    state_ = ApplyState::Applying;
}

void RuntimeProfileState::record_apply(const ApplyResult &result, const ExecutionPlan &plan,
                                       int64_t now_ms) {
    effective_intent_ = plan.effective_intent_id;
    capability_health_ = plan.readiness;
    device_validation_pending_ = plan.readiness == ExecutionReadiness::DeviceValidationRequired;
    rollback_failed_ = result.rollback_attempted && !result.rollback_succeeded;

    // The rule this whole class exists for: only a verified apply advances the verified
    // profile. Everything below chooses a state; only this branch moves what Flux claims.
    if (result.verified_active && !result.critical_failure) {
        verified_ = plan.requested_profile;
        effective_ = plan.requested_profile;
        has_verified_ = true;
        last_verified_apply_ms_ = now_ms;
        degraded_reason_.clear();

        // Applied — but "applied" and "got everything you asked for" are different claims.
        // A device whose vendor knobs are gated genuinely ran the generic plan, and genuinely
        // did not deliver the vendor part of the ask.
        switch (plan.readiness) {
            case ExecutionReadiness::Ready:
                state_ = ApplyState::Verified;
                fully_optimized_ = result.prevented_count == 0 && result.optional_failures == 0 &&
                                   plan.skipped_unsupported == 0;
                break;
            case ExecutionReadiness::Constrained:
            case ExecutionReadiness::TelemetryDegraded:
                state_ = ApplyState::Constrained;
                fully_optimized_ = false;
                break;
            default:
                state_ = ApplyState::CapabilityLimited;
                fully_optimized_ = false;
                break;
        }
        return;
    }

    // Nothing below here may touch verified_: the device did not do what was asked, so the last
    // thing Flux actually verified is still the last thing Flux actually verified.
    fully_optimized_ = false;

    if (result.degraded) {
        state_ = result.rollback_attempted && !result.rollback_succeeded ? ApplyState::RollbackFailed
                                                                         : ApplyState::Degraded;
        degraded_reason_ = result.message;
        return;
    }
    if (result.rollback_unavailable) {
        state_ = ApplyState::RestorationUnavailable;
        degraded_reason_ = result.message;
        return;
    }
    if (result.critical_failure || result.plan_rejected) {
        state_ = ApplyState::CapabilityLimited;
        degraded_reason_ = result.message;
        return;
    }
    if (plan.actions.empty()) {
        // Nothing was executable. Honest and quiet: no writes, no claim.
        state_ = ApplyState::Unsupported;
        degraded_reason_ = "no capability on this device serves the requested intent";
        return;
    }
    state_ = ApplyState::CapabilityLimited;
    degraded_reason_ = result.message;
}

void RuntimeProfileState::record_external_mutation(int64_t /*now_ms*/) {
    // Flux verified a value; the device no longer holds it. Whatever Flux last wrote, it can no
    // longer claim the profile is in place — so the claim is dropped rather than defended.
    state_ = ApplyState::ExternalMutation;
    has_verified_ = false;
    fully_optimized_ = false;
    degraded_reason_ = "a capability was changed outside Flux";
}

void RuntimeProfileState::record_restore(const ApplyResult &result, int64_t now_ms) {
    if (result.verified_active) {
        state_ = ApplyState::Idle;
        has_verified_ = false; // the device is back to its own values, not to a Flux profile
        fully_optimized_ = false;
        last_verified_apply_ms_ = now_ms;
        degraded_reason_.clear();
        return;
    }
    state_ = ApplyState::RestorationUnavailable;
    fully_optimized_ = false;
    degraded_reason_ = result.message;
}

} // namespace flux::execution
