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

// LivePlanCompiler + ExecutionEngine: compiling a dry run into an executable plan, and applying
// it transactionally.
//
// These drive the engine through an in-memory backend, which is the point of the NodeBackend
// contract: every transactional path — rollback, verify mismatch, mode-restore failure, a device
// that changes underneath a plan — is reachable on a laptop, with no root and no device.
//
// Capability probing, descriptor validation and value/type/allowlist checking are tested against
// a real filesystem tree in DeviceDescriptorTest and DryRunPlannerTest. Here the plan is
// hand-built, so the engine's behaviour is isolated from the planner's.

#include "TestFramework.hpp"

#include "ExecutionEngine.hpp"

#include <string>
#include <vector>

using namespace flux::execution;
using flux::engine::DecisionPriority;
using flux::engine::DecisionReason;
using flux::engine::TargetProfile;

namespace {

constexpr const char *kCpu = "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor";
constexpr const char *kGpu = "/sys/class/kgsl/kgsl-3d0/devfreq/governor";

PathPolicy test_policy() { return PathPolicy(std::vector<std::string>{"/sys/"}); }

DryRunAction action(std::string id, std::string path, std::string value, bool critical,
                    CapabilityState state = CapabilityState::Supported) {
    DryRunAction a;
    a.action_id = std::move(id);
    a.capability_id = a.action_id;
    a.descriptor_id = a.action_id;
    a.descriptor_set = "test";
    a.backend = "sysfs";
    a.target_path = std::move(path);
    a.desired_value = std::move(value);
    a.value_type = NodeValueType::Token;
    a.read_back = ReadBackStrategy::Exact;
    a.rollback = RollbackStrategy::RestoreOriginal;
    a.critical = critical;
    a.capability_state = state;
    a.validation = ValidationStatus::PhysicalDeviceValidated;
    a.source_intent_id = "sustained_performance";
    a.reason = DecisionReason::session_started;
    return a;
}

DryRunExecutionPlan dry_run(std::vector<DryRunAction> actions) {
    DryRunExecutionPlan plan;
    plan.valid = true;
    plan.requested.intent_id = "sustained_performance";
    plan.requested.source_profile = TargetProfile::Performance;
    plan.requested.reason = DecisionReason::session_started;
    plan.requested.priority = DecisionPriority::SessionLifecycle;
    plan.requested.health = flux::engine::DataHealth::Healthy;
    plan.effective_intent_id = "sustained_performance";
    plan.projected.readiness = ExecutionReadiness::Ready;
    plan.actions = std::move(actions);
    return plan;
}

/// A backend with both nodes present and writable.
InMemoryNodeBackend seeded() {
    InMemoryNodeBackend backend;
    backend.seed(kCpu, "schedutil");
    backend.seed(kGpu, "msm-adreno-tz");
    return backend;
}

ExecutionPlan compile(const DryRunExecutionPlan &plan, const NodeBackend &backend,
                      uint64_t generation = 1) {
    return LivePlanCompiler::compile(plan, backend, test_policy(), generation);
}

} // namespace

// --- compilation -----------------------------------------------------------

TEST("compile: a non-Supported capability can never become an executable action") {
    // The capability gate, restated where actions are actually produced. Every non-Supported
    // state must be refused, so this enumerates them rather than sampling one.
    const CapabilityState blocked[] = {
        CapabilityState::Unsupported,
        CapabilityState::Unavailable,
        CapabilityState::PermissionDenied,
        CapabilityState::InvalidFormat,
        CapabilityState::IncompleteGroup,
        CapabilityState::VerificationFailed,
        CapabilityState::DeviceValidationRequired,
        CapabilityState::ReadOnly,
        CapabilityState::PathRejected,
        CapabilityState::ExternalMutation,
    };

    for (const auto state : blocked) {
        auto backend = seeded();
        const auto plan =
            compile(dry_run({action("cpu", kCpu, "performance", true, state)}), backend);

        CHECK_MSG(!plan.valid, std::string("a ") + capability_state_name(state) +
                                   " capability must never compile into an executable plan");
        CHECK_EQ(plan.rejection, PlanRejection::CapabilityNotSupported);
        CHECK_MSG(plan.actions.empty(), "a rejected plan must carry no actions at all");
        CHECK_MSG(backend.total_writes() == 0, "compiling must never write");
        CHECK_MSG(backend.total_chmods() == 0, "compiling must never chmod");
    }
}

TEST("compile: a target outside the approved roots is rejected") {
    auto backend = seeded();
    backend.seed("/etc/passwd", "root");
    const auto plan = compile(dry_run({action("bad", "/etc/passwd", "x", true)}), backend);

    CHECK(!plan.valid);
    CHECK_EQ(plan.rejection, PlanRejection::UnsafePath);
    CHECK_EQ(backend.total_writes(), 0);
}

TEST("compile: a path-traversal target is rejected") {
    auto backend = seeded();
    backend.seed("/sys/../etc/passwd", "root");
    const auto plan = compile(dry_run({action("bad", "/sys/../etc/passwd", "x", true)}), backend);

    CHECK(!plan.valid);
    CHECK_EQ(plan.rejection, PlanRejection::UnsafePath);
}

TEST("compile: an invalid desired value invalidates the whole plan") {
    auto backend = seeded();
    auto bad = action("cpu", kCpu, "not-a-number", true);
    bad.value_type = NodeValueType::Integer;

    const auto plan = compile(dry_run({bad}), backend);

    CHECK(!plan.valid);
    CHECK_EQ(plan.rejection, PlanRejection::InvalidValue);
    CHECK_MSG(plan.actions.empty(), "one bad value must not leave the other actions executable");
}

TEST("compile: two actions wanting different values on one node are rejected") {
    // Whichever ran last would win, so the outcome would depend on ordering rather than content.
    auto backend = seeded();
    const auto plan = compile(
        dry_run({action("a", kCpu, "performance", true), action("b", kCpu, "powersave", true)}),
        backend);

    CHECK(!plan.valid);
    CHECK_EQ(plan.rejection, PlanRejection::ConflictingTargets);
    CHECK_EQ(backend.total_writes(), 0);
}

TEST("compile: identical values on one node are not a conflict") {
    auto backend = seeded();
    const auto plan = compile(
        dry_run({action("a", kCpu, "performance", true), action("b", kCpu, "performance", false)}),
        backend);

    CHECK_MSG(plan.valid,
              "two actions agreeing on a value is not a conflict: " + plan.invalid_reason);
}

TEST("compile: a dependency on an action that is not in the plan is rejected") {
    auto backend = seeded();
    auto dependent = action("gpu", kGpu, "performance", false);
    dependent.depends_on = {"a-descriptor-that-was-prevented"};

    const auto plan = compile(dry_run({dependent}), backend);

    CHECK(!plan.valid);
    CHECK_EQ(plan.rejection, PlanRejection::MissingDependency);
}

TEST("compile: an action depending on itself is a cycle, not a dependency") {
    auto backend = seeded();
    auto looped = action("gpu", kGpu, "performance", false);
    looped.depends_on = {"gpu"};

    const auto plan = compile(dry_run({looped}), backend);

    CHECK(!plan.valid);
    CHECK_EQ(plan.rejection, PlanRejection::DependencyCycle);
}

TEST("compile: a required rollback with no readable original is refused before any write") {
    // The engine must never invent a rollback value. If the original cannot be read, the only
    // safe answer is to not perform the action at all.
    InMemoryNodeBackend backend; // nothing seeded: reads return nullopt
    const auto plan = compile(dry_run({action("cpu", kCpu, "performance", true)}), backend);

    CHECK(!plan.valid);
    CHECK_EQ(plan.rejection, PlanRejection::RollbackImpossible);
    CHECK_EQ(backend.total_writes(), 0);
}

TEST("compile: actions are ordered deterministically by (order_group, action_id)") {
    auto backend = seeded();
    backend.seed("/sys/a", "0");
    backend.seed("/sys/b", "0");
    backend.seed("/sys/c", "0");

    auto late = action("z-late", "/sys/a", "1", false);
    late.order_group = 30;
    auto early = action("m-early", "/sys/b", "1", false);
    early.order_group = 10;
    auto middle = action("a-middle", "/sys/c", "1", false);
    middle.order_group = 20;

    const auto plan = compile(dry_run({late, early, middle}), backend);

    CHECK(plan.valid);
    CHECK_EQ(plan.actions.size(), static_cast<size_t>(3));
    CHECK_EQ(plan.actions[0].action_id, std::string("m-early"));
    CHECK_EQ(plan.actions[1].action_id, std::string("a-middle"));
    CHECK_EQ(plan.actions[2].action_id, std::string("z-late"));
}

TEST("compile: a dry run with nothing to do is a valid no-op, not a failure") {
    auto backend = seeded();
    const auto plan = compile(dry_run({}), backend);

    CHECK_MSG(plan.valid, "a device with no supported capability is not a broken plan");
    CHECK(plan.actions.empty());
}

TEST("compile: an invalid dry run never becomes an executable plan") {
    auto backend = seeded();
    auto invalid = dry_run({action("cpu", kCpu, "performance", true)});
    invalid.valid = false;
    invalid.invalid_reason = "planner said no";

    const auto plan = compile(invalid, backend);

    CHECK(!plan.valid);
    CHECK_EQ(plan.rejection, PlanRejection::NotExecutable);
    CHECK(plan.actions.empty());
}

// --- apply -----------------------------------------------------------------

TEST("apply: a successful apply writes, verifies, and reports active") {
    auto backend = seeded();
    ExecutionEngine engine(backend);
    const auto plan = compile(
        dry_run({action("cpu", kCpu, "performance", true), action("gpu", kGpu, "performance", false)}),
        backend);

    const auto result = engine.apply(plan, "balance", 1, 1000);

    CHECK_MSG(result.verified_active, "every critical action verified: " + result.message);
    CHECK(!result.critical_failure);
    CHECK_EQ(result.succeeded, 2);
    CHECK_EQ(backend.read(kCpu).value(), std::string("performance"));
    CHECK_EQ(backend.read(kGpu).value(), std::string("performance"));
}

TEST("apply: repeating an identical decision performs zero writes") {
    auto backend = seeded();
    ExecutionEngine engine(backend);
    const auto plan = compile(dry_run({action("cpu", kCpu, "performance", true)}), backend);

    (void)engine.apply(plan, "balance", 1, 1000);
    backend.reset_counts();
    const auto again = engine.apply(plan, "performance", 1, 2000);

    CHECK_MSG(backend.total_writes() == 0, "an unchanged decision must not rewrite the node");
    CHECK_MSG(backend.total_chmods() == 0, "an unchanged decision must not chmod");
    CHECK_EQ(again.skipped_idempotent, 1);
    CHECK_MSG(again.verified_active, "a no-op apply is still a verified apply");
}

TEST("apply: invalidation forces a rewrite (restart, config change, capability change)") {
    auto backend = seeded();
    ExecutionEngine engine(backend);
    const auto plan = compile(dry_run({action("cpu", kCpu, "performance", true)}), backend);

    (void)engine.apply(plan, "balance", 1, 1000);
    engine.invalidate_all();
    backend.reset_counts();
    const auto again = engine.apply(plan, "performance", 1, 2000);

    CHECK_MSG(backend.write_count(kCpu) == 1, "an invalidated value must be written again");
    CHECK_EQ(again.skipped_idempotent, 0);
}

TEST("apply: a plan compiled against a stale capability generation is refused") {
    // Time-of-check/time-of-use: the device changed after the plan was inspected, so every
    // conclusion in it is suspect.
    auto backend = seeded();
    ExecutionEngine engine(backend);
    const auto plan = compile(dry_run({action("cpu", kCpu, "performance", true)}), backend, 7);

    const auto result = engine.apply(plan, "balance", /*live generation=*/8, 1000);

    CHECK(result.plan_rejected);
    CHECK_EQ(result.rejection, PlanRejection::CapabilityGenerationChanged);
    CHECK(!result.verified_active);
    CHECK_MSG(backend.total_writes() == 0, "a stale plan must not write anything");
}

TEST("apply: a critical read-back mismatch fails and rolls the group back") {
    // The node accepted the write and ignored the value — the most common way a sysfs tuning
    // attempt silently does nothing.
    auto backend = seeded();
    backend.ignore_value(kCpu, "performance");
    ExecutionEngine engine(backend);
    const auto plan = compile(dry_run({action("cpu", kCpu, "performance", true)}), backend);

    const auto result = engine.apply(plan, "balance", 1, 1000);

    CHECK_MSG(!result.verified_active, "a value the node did not take is not active");
    CHECK(result.critical_failure);
    CHECK_EQ(result.worst_error, NodeError::VerifyMismatch);
    CHECK(result.rollback_attempted);
    CHECK_EQ(backend.read(kCpu).value(), std::string("schedutil"));
}

TEST("apply: a critical write failure rolls back the whole critical group") {
    auto backend = seeded();
    backend.seed("/sys/first", "original");
    backend.fail_writes_to(kCpu, NodeError::PermissionDenied);
    ExecutionEngine engine(backend);

    auto first = action("first", "/sys/first", "changed", true);
    first.order_group = 1;
    auto second = action("cpu", kCpu, "performance", true);
    second.order_group = 2;

    const auto result = engine.apply(compile(dry_run({first, second}), backend), "balance", 1, 1000);

    CHECK(result.critical_failure);
    CHECK(!result.verified_active);
    CHECK_EQ(result.worst_error, NodeError::PermissionDenied);
    CHECK(result.rollback_attempted);
    CHECK_MSG(result.rollback_succeeded, "the earlier critical write must be undone");
    CHECK_MSG(backend.read("/sys/first").value() == std::string("original"),
              "a critical group is applied whole or not at all");
}

TEST("apply: EACCES and EROFS are failures, never success") {
    for (const auto error : {NodeError::PermissionDenied, NodeError::ReadOnlyFilesystem}) {
        auto backend = seeded();
        backend.fail_writes_to(kCpu, error);
        ExecutionEngine engine(backend);
        const auto plan = compile(dry_run({action("cpu", kCpu, "performance", true)}), backend);

        const auto result = engine.apply(plan, "balance", 1, 1000);

        CHECK_MSG(!result.verified_active, std::string(node_error_name(error)) +
                                               " must never be reported as an applied profile");
        CHECK(result.critical_failure);
        CHECK_EQ(result.worst_error, error);
    }
}

TEST("apply: an optional write failure is recorded but not fatal") {
    auto backend = seeded();
    backend.fail_writes_to(kGpu, NodeError::WriteFailed);
    ExecutionEngine engine(backend);
    const auto plan = compile(
        dry_run({action("cpu", kCpu, "performance", true), action("gpu", kGpu, "performance", false)}),
        backend);

    const auto result = engine.apply(plan, "balance", 1, 1000);

    CHECK_MSG(result.verified_active, "an optional failure does not stop the profile");
    CHECK(!result.critical_failure);
    CHECK_EQ(result.optional_failures, 1);
    CHECK_EQ(result.succeeded, 1);
    CHECK_EQ(backend.read(kCpu).value(), std::string("performance"));
}

TEST("apply: rollback that cannot restore a value reports degraded, never success") {
    auto backend = seeded();
    backend.ignore_value(kCpu, "performance"); // the write lands but the node ignores it
    // ...and the node will not take the old value back either. The write succeeds, the verify
    // fails, and the rollback then fails too: the node is stuck at a value nobody chose.
    backend.fail_writes_of_value(kCpu, "schedutil", NodeError::WriteFailed);
    ExecutionEngine engine(backend);

    const auto plan = compile(dry_run({action("cpu", kCpu, "performance", true)}), backend);

    const auto result = engine.apply(plan, "balance", 1, 1000);

    CHECK(result.critical_failure);
    CHECK(result.rollback_attempted);
    CHECK_MSG(!result.rollback_succeeded, "the rollback write failed");
    CHECK_MSG(result.degraded, "a failed rollback leaves a state nobody designed; say so");
    CHECK(!result.verified_active);
    CHECK_EQ(result.degraded_capability, std::string("cpu"));
}

TEST("apply: a mode-restore failure is surfaced, not swallowed") {
    // The value landed but the node's permissions could not be put back. The legacy applier
    // could not even detect this; reporting it is the whole point of NodeWriteResult.
    auto backend = seeded();
    backend.fail_mode_restore(kCpu);
    ExecutionEngine engine(backend);
    const auto plan = compile(dry_run({action("cpu", kCpu, "performance", true)}), backend);

    const auto result = engine.apply(plan, "balance", 1, 1000);

    CHECK_EQ(result.worst_error, NodeError::ModeRestoreFailed);
    CHECK(result.critical_failure);
    CHECK_MSG(!result.verified_active,
              "a node left with the wrong mode is not a cleanly applied profile");
}

TEST("apply: a no-op plan writes nothing and claims nothing") {
    auto backend = seeded();
    ExecutionEngine engine(backend);
    const auto plan = compile(dry_run({}), backend);

    const auto result = engine.apply(plan, "balance", 1, 1000);

    CHECK_EQ(backend.total_writes(), 0);
    CHECK_EQ(backend.total_chmods(), 0);
    CHECK_MSG(!result.verified_active, "nothing was applied, so nothing may be reported as verified");
}

TEST("apply: a rejected plan is refused without any writes") {
    auto backend = seeded();
    ExecutionEngine engine(backend);
    auto invalid = dry_run({action("cpu", kCpu, "performance", true)});
    invalid.valid = false;

    const auto result = engine.apply(compile(invalid, backend), "balance", 1, 1000);

    CHECK(result.plan_rejected);
    CHECK(!result.verified_active);
    CHECK_EQ(backend.total_writes(), 0);
    CHECK_EQ(backend.total_chmods(), 0);
}

TEST("apply: a critical capability that vanished after planning is caught in preflight") {
    auto backend = seeded();
    const auto plan = compile(
        dry_run({action("cpu", kCpu, "performance", true), action("gpu", kGpu, "performance", false)}),
        backend);

    // The node disappears between compiling and applying — a real event on a device where a
    // driver unloads or a CPU policy is hot-unplugged.
    InMemoryNodeBackend gone;
    gone.seed(kGpu, "msm-adreno-tz");
    ExecutionEngine engine_without_cpu(gone);

    const auto result = engine_without_cpu.apply(plan, "balance", 1, 1000);

    CHECK(result.plan_rejected);
    CHECK(result.critical_failure);
    CHECK_MSG(gone.total_writes() == 0,
              "no critical write may happen until the whole group passes preflight");
}

// --- external mutation, restore, history -----------------------------------

TEST("external mutation: a drifted value is detected and forgotten, not trusted") {
    auto backend = seeded();
    ExecutionEngine engine(backend);
    const auto plan = compile(dry_run({action("cpu", kCpu, "performance", true)}), backend);
    (void)engine.apply(plan, "balance", 1, 1000);

    backend.external_change(kCpu, "powersave"); // something outside Flux moved it

    const auto drifted = engine.detect_external_mutation();

    CHECK_EQ(drifted.size(), static_cast<size_t>(1));
    CHECK_EQ(drifted[0], std::string("cpu"));
    CHECK_MSG(!engine.verified_value("cpu").has_value(),
              "a cached value the device no longer holds must be forgotten, not believed");

    // And the next apply must actually rewrite it rather than skipping it as already done.
    backend.reset_counts();
    (void)engine.apply(plan, "balance", 1, 2000);
    CHECK_EQ(backend.write_count(kCpu), 1);
}

TEST("external mutation: an untouched value is not reported as drifted") {
    auto backend = seeded();
    ExecutionEngine engine(backend);
    const auto plan = compile(dry_run({action("cpu", kCpu, "performance", true)}), backend);
    (void)engine.apply(plan, "balance", 1, 1000);

    CHECK(engine.detect_external_mutation().empty());
}

TEST("restore: first-seen originals are captured and restored exactly") {
    auto backend = seeded();
    ExecutionEngine engine(backend);

    (void)engine.apply(compile(dry_run({action("cpu", kCpu, "performance", true)}), backend),
                       "balance", 1, 1000);
    // A second apply must not overwrite the *true* original with an intermediate value.
    (void)engine.apply(compile(dry_run({action("cpu", kCpu, "powersave", true)}), backend),
                       "performance", 1, 2000);

    const auto result = engine.restore_originals("session_ended", 3000);

    CHECK(result.verified_active);
    CHECK_MSG(backend.read(kCpu).value() == std::string("schedutil"),
              "restore must return the value the device had before Flux ever touched it");
}

TEST("history: the ring buffer is bounded and evicts the oldest entry") {
    auto backend = seeded();
    ExecutionEngine engine(backend, /*history_capacity=*/3);
    const auto plan = compile(dry_run({action("cpu", kCpu, "performance", true)}), backend);

    for (int i = 0; i < 10; ++i) {
        engine.invalidate_all();
        (void)engine.apply(plan, "balance", 1, 1000 + i);
    }

    CHECK_MSG(engine.history().size() == 3, "history must stay bounded, whatever the device does");
    CHECK_MSG(engine.history().front().monotonic_ms == 1007, "the oldest entry is the one evicted");
    CHECK_EQ(engine.history().back().monotonic_ms, static_cast<int64_t>(1009));
}

TEST("history: records carry counts and a sanitized error category, never a path") {
    auto backend = seeded();
    backend.fail_writes_to(kCpu, NodeError::PermissionDenied);
    ExecutionEngine engine(backend);
    const auto plan = compile(dry_run({action("cpu", kCpu, "performance", true)}), backend);

    (void)engine.apply(plan, "balance", 1, 1000);

    const auto &entry = engine.history().back();
    CHECK_EQ(entry.error_category, std::string("permission_denied"));
    CHECK(entry.critical_failure);
    CHECK(!entry.verified_active);
    CHECK(entry.rollback_attempted);
    CHECK_EQ(entry.action_count, 1);
    CHECK_EQ(entry.requested_profile, std::string("performance"));
    CHECK_EQ(entry.telemetry_health, std::string("healthy"));
    CHECK_MSG(entry.error_category.find('/') == std::string::npos,
              "history is exportable diagnostics: it must carry a category, never a device path");
}
