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
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <vector>

#include "DeviceDescriptor.hpp"
#include "DryRunPlanner.hpp"
#include "ExecutionEngine.hpp"
#include "PolicyIntent.hpp"
#include "RuntimeProfileState.hpp"
#include "RuntimeTuning.hpp"
#include "SysfsNodeBackend.hpp"
#include "ZenController.hpp"

/**
 * @file ExecutionRuntime.hpp
 * @brief The live execution composition root: the daemon's single profile-apply entry point.
 *
 * Flux-owned (Category A).
 *
 * ## What this is for
 *
 * Everything needed to turn a Decision into verified device state, owned in one place with an
 * explicit lifetime. Before this existed, applying a profile meant Main.cpp calling a Profiler
 * function that shelled out to flux_profiler.sh — the decision, the plan, the write and the
 * claim of success were spread across a C++ file, a shell script and nothing at all.
 *
 * There is exactly one of these in the process, it is constructed after telemetry and destroyed
 * before it, and it owns the only ExecutionEngine and the only NodeBackend. Main.cpp routes
 * events to it and reads state from it; Main.cpp does not write device nodes, and it does not
 * decide anything.
 *
 * ## Ownership and lifetime
 *
 * Construction order is fixed by member order: backend -> probe -> planner -> engine -> state.
 * Each member depends only on the ones above it, so there is no initialization order to get
 * wrong and no two-phase init. Destruction is the reverse, automatically.
 *
 * There are no threads here. The runtime does exactly what its caller asks, on the caller's
 * thread, and returns — so there is no worker that can fire a callback into a destroyed object,
 * which is the failure this design avoids by not having the thread rather than by joining it.
 * After shutdown() the runtime refuses to write; the flag is checked at the one entry point
 * that writes rather than trusted to the caller.
 */
namespace flux::execution {

/** What the daemon needs to know about the session, to serve an intent that mentions one. */
struct SessionContext {
    bool in_session = false;
    std::string package;
    int pid = 0;
    int uid = 0;
    bool wants_zen = false; ///< the registry says this game asked for Do-Not-Disturb
    int desired_zen_mode = 1;
};

/** The outcome of one runtime cycle. */
struct RuntimeCycleResult {
    bool planned = false;   ///< a plan was compiled
    bool applied = false;   ///< the engine ran
    bool coalesced = false; ///< nothing meaningful changed; no plan was built
    ApplyResult apply;
    ExecutionPlan plan;
};

/** Publishes the runtime's state for the WebUI and CLI. Injected so tests write no files. */
using StatusPublisher = std::function<void(const RuntimeProfileState &, const SessionContext &)>;

/**
 * @brief Owns and drives the live execution pipeline.
 */
class ExecutionRuntime {
public:
    /**
     * @param packs the device packs to resolve intents against. Vendor packs are gated by their
     *        own validation status; passing them does not make them executable.
     * @param identity the detected SoC. Used only to select which packs apply.
     */
    ExecutionRuntime(std::vector<DevicePack> packs, DeviceIdentity identity,
                     PathPolicy policy = PathPolicy{}, ZenBackend *zen = nullptr,
                     size_t history_capacity = 64);

    /**
     * @brief Run one cycle: decision -> intent -> plan -> compile -> apply -> state.
     *
     * The single production path from a policy decision to a device write. Coalesces work that
     * would change nothing, but never coalesces a safety downgrade.
     */
    RuntimeCycleResult on_decision(const flux::engine::Decision &decision,
                                   const SessionContext &session, int64_t now_ms);

    /** Put every value Flux ever changed back, exactly. Safe to call more than once. */
    ApplyResult restore_all(const std::string &reason, int64_t now_ms);

    /**
     * @brief Stop accepting work. After this, no cycle may write.
     *
     * Idempotent, and does not itself write: restoring is the caller's decision, made before
     * shutting the runtime down, because "put the device back" is policy and this is mechanism.
     */
    void shutdown();
    [[nodiscard]] bool is_shut_down() const { return shut_down_; }

    /**
     * @brief Install migrated configuration.
     *
     * Applies the user's governor choices and mitigation suppressions to the packs, and sets the
     * master gate. Bumps the capability generation: what the engine believes it verified was
     * verified under the old settings.
     *
     * When tweaks are disabled this restores everything Flux changed, once, and then applies
     * nothing until they are enabled again. A user who turned Flux off gets their device back,
     * not merely an absence of further changes.
     */
    void set_tuning(RuntimeTuning tuning, int64_t now_ms);

    [[nodiscard]] const RuntimeTuning &tuning() const { return tuning_; }
    [[nodiscard]] bool tweaks_enabled() const { return tuning_.tweaks_enabled; }

    /// Bump when the device's capabilities may have changed: a config change, a descriptor
    /// change, a detected external mutation, or an explicit reapply. Invalidates the idempotency
    /// cache, so the next cycle writes rather than assuming its last values still hold.
    void invalidate_capabilities(const std::string &reason);

    /// Re-read what Flux believes it verified. Any drift invalidates the cache and moves the
    /// state to ExternalMutation, so the next cycle re-applies instead of trusting the cache.
    /// @return the capability ids that had drifted.
    std::vector<std::string> poll_external_mutation(int64_t now_ms);

    void set_status_publisher(StatusPublisher publisher) { publisher_ = std::move(publisher); }

    [[nodiscard]] const RuntimeProfileState &state() const { return state_; }
    [[nodiscard]] const std::deque<ApplyHistoryEntry> &history() const { return engine_.history(); }
    [[nodiscard]] uint64_t capability_generation() const { return capability_generation_; }
    [[nodiscard]] const ZenController *zen() const { return zen_ ? &*zen_ : nullptr; }

private:
    // Declaration order is construction order, and each depends only on the ones above it.
    PathPolicy policy_;
    std::vector<DevicePack> base_packs_; ///< as supplied; tuning is re-applied from these
    std::vector<DevicePack> packs_;      ///< what the planner actually sees
    DeviceIdentity identity_;
    SysfsNodeBackend backend_;
    CapabilityProbe probe_;
    DryRunPlanner planner_;
    ExecutionEngine engine_;
    RuntimeProfileState state_;
    std::optional<ZenController> zen_;

    RuntimeTuning tuning_;
    StatusPublisher publisher_;
    SessionContext last_session_; ///< what the last cycle was told, for the status publisher
    uint64_t capability_generation_ = 1;
    bool shut_down_ = false;

    /// The last ask that was actually planned, for coalescing.
    std::string last_intent_id_;
    bool have_last_intent_ = false;
    uint64_t last_intent_generation_ = 0;

    bool zen_engaged_for_session_ = false;

    void drive_zen(const CapabilityIntentSet &intents, const SessionContext &session);
    void publish();
};

} // namespace flux::execution
