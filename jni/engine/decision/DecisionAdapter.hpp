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

#include <ProfilePolicy.hpp> // PolicyInputs / PolicyState / PolicyDecision boundary types
#include <SynthesisCore.hpp> // TelemetrySnapshot / TelemetryHealth

#include "DecisionEngine.hpp"

/**
 * @file DecisionAdapter.hpp
 * @brief Short-lived compatibility boundary between the daemon and the Flux V2
 *        Decision Engine.
 *
 * ## Purpose
 *
 * The daemon still speaks the legacy vocabulary (PolicyInputs / PolicyState /
 * PolicyDecision, FluxProfileMode, TransitionReason) in its apply, diagnostics
 * and record paths. This adapter lets the V2 `flux::engine::DecisionEngine`
 * become the *sole* runtime decision maker without a wholesale rewrite of those
 * paths: it accepts the daemon's inputs, evaluates the pure V2 engine, and maps
 * the result back to a PolicyDecision. The Encore-derived `ProfilePolicy::evaluate`
 * is no longer called on the runtime decision path.
 *
 * ## This is deliberately a compatibility adapter
 *
 *  - **Removal condition:** delete this once the daemon's record/diagnostics
 *    types are migrated to Flux V2-native vocabulary (Telemetry Integration /
 *    Diagnostics phase). At that point Main.cpp consumes `flux::engine::Decision`
 *    directly and this file, plus the legacy ProfilePolicy types, go away.
 *  - **No new feature development happens here.** It only translates.
 *  - **Tested:** the translation is covered by host tests (parity harness).
 */
class FluxDecisionService {
public:
    explicit FluxDecisionService(flux::engine::DecisionConfig config = {}) : engine_(config) {}

    /**
     * @brief Decide the profile for this cycle, in the daemon's legacy shape.
     *
     * @p state.current is treated as authoritative on entry (so a caller that
     * rolled it back after a failed apply is respected), and is written back to
     * reflect the new decision.
     */
    [[nodiscard]] PolicyDecision decide(const PolicyInputs &inputs, PolicyState &state,
                                        int64_t now_ms);

    /** Expose the last full V2 decision for richer diagnostics if a caller wants it. */
    [[nodiscard]] const flux::engine::Decision &last_decision() const { return last_; }

private:
    flux::engine::DecisionEngine engine_;
    flux::engine::EngineState engine_state_;
    flux::engine::Decision last_;
};

// Pure mapping helpers, exposed for testing.
namespace flux::engine::compat {

FluxProfileMode to_flux_profile(TargetProfile profile);
TargetProfile from_flux_profile(FluxProfileMode profile);
TransitionReason to_transition_reason(DecisionReason reason);

/** Build a validated V2 RuntimeSnapshot from the daemon's telemetry inputs. */
RuntimeSnapshot build_runtime_snapshot(TelemetryHealth health,
                                       const std::optional<TelemetrySnapshot> &snapshot);

/** Derive the V2 capability view from the daemon's telemetry inputs. */
CapabilitySnapshot build_capabilities(const std::optional<TelemetrySnapshot> &snapshot);

} // namespace flux::engine::compat
