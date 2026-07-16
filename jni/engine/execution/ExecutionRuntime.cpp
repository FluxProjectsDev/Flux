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

#include "ExecutionRuntime.hpp"

#include <utility>

namespace flux::execution {

using flux::engine::Decision;
using flux::engine::DecisionPriority;

ExecutionRuntime::ExecutionRuntime(std::vector<DevicePack> packs, DeviceIdentity identity,
                                   PathPolicy policy, ZenBackend *zen, size_t history_capacity)
    : policy_(std::move(policy)),
      packs_(std::move(packs)),
      identity_(std::move(identity)),
      backend_(policy_),
      probe_(backend_, policy_),
      planner_(probe_),
      engine_(backend_, history_capacity) {
    if (zen != nullptr) zen_.emplace(*zen);
}

void ExecutionRuntime::shutdown() {
    shut_down_ = true;
}

void ExecutionRuntime::invalidate_capabilities(const std::string &reason) {
    (void)reason;
    ++capability_generation_;
    engine_.invalidate_all();
    have_last_intent_ = false;
}

std::vector<std::string> ExecutionRuntime::poll_external_mutation(int64_t now_ms) {
    if (shut_down_) return {};

    auto drifted = engine_.detect_external_mutation();
    if (!drifted.empty()) {
        // The device no longer holds what Flux verified. Anything cached about it is now a
        // guess, so the generation moves and the next cycle re-plans against the real device.
        state_.record_external_mutation(now_ms);
        ++capability_generation_;
        have_last_intent_ = false;
        publish();
    }
    return drifted;
}

void ExecutionRuntime::drive_zen(const CapabilityIntentSet &intents, const SessionContext &session) {
    if (!zen_) return; // no zen capability on this device: nothing is touched

    const bool restoring = intents.policy.restoration_required ||
                           intents.policy.behavior == BehaviorClass::Restore;
    const bool wants_zen = session.wants_zen && session.in_session && !restoring;

    if (wants_zen && !zen_engaged_for_session_) {
        zen_engaged_for_session_ = zen_->engage(session.desired_zen_mode);
        return;
    }
    if (!wants_zen && zen_engaged_for_session_) {
        // restore() puts the exact original integer back, and declines if the user has since
        // changed zen themselves — their choice outranks Flux's cleanup.
        (void)zen_->restore();
        zen_engaged_for_session_ = false;
    }
}

RuntimeCycleResult ExecutionRuntime::on_decision(const Decision &decision,
                                                 const SessionContext &session, int64_t now_ms) {
    RuntimeCycleResult out;
    if (shut_down_) return out; // no write may happen once shutdown has begun

    const auto intents = IntentMapper::from_decision(decision);
    const std::string intent_id = intents.policy.intent_id;

    // Coalescing. A repeated identical ask against an unchanged device is not work. The engine
    // would reach the same conclusion by way of its idempotency cache, but building and
    // compiling a plan to discover that is waste on every tick of a quiet device.
    //
    // A safety response is never coalesced, whatever the last ask was: the whole point of
    // immediate_downgrade_required is that it may not wait for a state comparison to agree.
    const bool safety = intents.policy.immediate_downgrade_required ||
                        intents.policy.priority <= DecisionPriority::TelemetrySafety;
    const bool same_ask = have_last_intent_ && last_intent_id_ == intent_id &&
                          last_intent_generation_ == capability_generation_;
    if (same_ask && !safety && apply_state_is_settled(state_.state())) {
        out.coalesced = true;
        return out;
    }

    state_.begin_planning(decision.desired_profile, intent_id, decision.health, now_ms);

    const auto dry = planner_.plan(intents, packs_, identity_, state_.verified_profile(), now_ms);
    out.plan = LivePlanCompiler::compile(dry, backend_, policy_, capability_generation_);
    out.planned = true;

    state_.begin_applying(out.plan, now_ms);
    out.apply = engine_.apply(out.plan, flux::engine::target_profile_name(state_.verified_profile()),
                              capability_generation_, now_ms);
    out.applied = true;

    state_.record_apply(out.apply, out.plan, now_ms);

    last_intent_id_ = intent_id;
    last_intent_generation_ = capability_generation_;
    have_last_intent_ = true;
    last_session_ = session;

    drive_zen(intents, session);
    publish();
    return out;
}

ApplyResult ExecutionRuntime::restore_all(const std::string &reason, int64_t now_ms) {
    ApplyResult result;
    if (shut_down_) return result;

    if (zen_ && zen_engaged_for_session_) {
        (void)zen_->restore();
        zen_engaged_for_session_ = false;
    }

    result = engine_.restore_originals(reason, now_ms);
    state_.record_restore(result, now_ms);
    have_last_intent_ = false;
    publish();
    return result;
}

void ExecutionRuntime::publish() {
    if (publisher_) publisher_(state_, last_session_);
}

} // namespace flux::execution
