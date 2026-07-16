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

// Host tests for the Flux V2 Execution Engine. An in-memory node backend stands in for
// sysfs, so capability, planning, transactional apply, verification, rollback,
// idempotency and restoration are all tested without root or a device.

#include "TestFramework.hpp"

#include "ExecutionEngine.hpp"

using namespace flux::execution;

namespace {

NodeDescriptor governor_node(bool critical = true) {
    NodeDescriptor d;
    d.id = "cpu.policy0.governor";
    d.path = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor";
    d.readable = true;
    d.writable = true;
    d.type = ValueType::Enum;
    d.allowed = {"performance", "schedutil", "powersave"};
    d.critical = critical;
    d.order_group = 0;
    return d;
}

NodeDescriptor freq_node() {
    NodeDescriptor d;
    d.id = "cpu.policy0.max_freq";
    d.path = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq";
    d.readable = true;
    d.writable = true;
    d.type = ValueType::IntRange;
    d.min = 300000;
    d.max = 3000000;
    d.critical = false;
    d.order_group = 1;
    return d;
}

ProfilePlanSpec spec(std::vector<ProfilePlanSpec::Item> items) { return ProfilePlanSpec{std::move(items)}; }

} // namespace

// --- Capability registry & validation --------------------------------------

TEST("capability: a present node is supported, an absent one is not") {
    InMemoryNodeBackend backend;
    backend.seed(governor_node().path, "schedutil");
    CapabilityRegistry reg;
    reg.register_node(governor_node(), backend);
    reg.register_node(freq_node(), backend); // path not seeded -> absent

    CHECK(reg.supported("cpu.policy0.governor"));
    CHECK(!reg.supported("cpu.policy0.max_freq"));
    CHECK_EQ(reg.unsupported_ids().size(), 1u);
}

TEST("validation: unsafe paths are rejected") {
    CHECK(ValueValidator::validate_path("/sys/ok/node") == ValidationError::Ok);
    CHECK(ValueValidator::validate_path("relative/path") == ValidationError::UnsafePath);
    CHECK(ValueValidator::validate_path("/sys/../etc/passwd") == ValidationError::UnsafePath);
    CHECK(ValueValidator::validate_path("") == ValidationError::UnsafePath);
}

TEST("validation: values are checked against type, range and allowlist") {
    NodeDescriptor gov = governor_node();
    CHECK(ValueValidator::validate_value(gov, "performance") == ValidationError::Ok);
    CHECK(ValueValidator::validate_value(gov, "turbo") == ValidationError::NotInAllowlist);

    NodeDescriptor freq = freq_node();
    CHECK(ValueValidator::validate_value(freq, "1800000") == ValidationError::Ok);
    CHECK(ValueValidator::validate_value(freq, "9999999") == ValidationError::OutOfRange);
    CHECK(ValueValidator::validate_value(freq, "fast") == ValidationError::TypeMismatch);
}

// --- Planning --------------------------------------------------------------

TEST("plan: unsupported capabilities are skipped, not fatal") {
    InMemoryNodeBackend backend;
    backend.seed(governor_node().path, "schedutil");
    CapabilityRegistry reg;
    reg.register_node(governor_node(), backend);
    reg.register_node(freq_node(), backend); // absent

    ExecutionPlan p = ExecutionPlanner{}.plan(
        spec({{"cpu.policy0.governor", "performance", "perf"},
              {"cpu.policy0.max_freq", "1800000", "perf"}}),
        reg, backend);
    CHECK(p.valid);
    CHECK_EQ(p.actions.size(), 1u);
    CHECK_EQ(p.skipped_unsupported, 1);
}

TEST("plan: an invalid value invalidates the whole plan") {
    InMemoryNodeBackend backend;
    backend.seed(governor_node().path, "schedutil");
    CapabilityRegistry reg;
    reg.register_node(governor_node(), backend);
    ExecutionPlan p = ExecutionPlanner{}.plan(
        spec({{"cpu.policy0.governor", "turbo", "bad"}}), reg, backend);
    CHECK(!p.valid);
    CHECK(p.actions.empty());
}

TEST("plan: conflicting duplicate actions are rejected") {
    InMemoryNodeBackend backend;
    backend.seed(governor_node().path, "schedutil");
    CapabilityRegistry reg;
    reg.register_node(governor_node(), backend);
    ExecutionPlan p = ExecutionPlanner{}.plan(
        spec({{"cpu.policy0.governor", "performance", "a"},
              {"cpu.policy0.governor", "powersave", "b"}}),
        reg, backend);
    CHECK(!p.valid);
}

TEST("plan: actions are ordered deterministically by group") {
    InMemoryNodeBackend backend;
    backend.seed(governor_node().path, "schedutil");
    backend.seed(freq_node().path, "1000000");
    CapabilityRegistry reg;
    reg.register_node(freq_node(), backend);     // order_group 1
    reg.register_node(governor_node(false), backend); // order_group 0
    ExecutionPlan p = ExecutionPlanner{}.plan(
        spec({{"cpu.policy0.max_freq", "1800000", "x"},
              {"cpu.policy0.governor", "performance", "x"}}),
        reg, backend);
    CHECK(p.valid);
    CHECK_EQ(p.actions.size(), 2u);
    CHECK_EQ(p.actions[0].capability_id, std::string("cpu.policy0.governor")); // group 0 first
}

// --- Apply / verify / idempotency ------------------------------------------

ExecutionPlan perf_plan(CapabilityRegistry &reg, InMemoryNodeBackend &backend) {
    return ExecutionPlanner{}.plan(spec({{"cpu.policy0.governor", "performance", "game"}}), reg, backend);
}

TEST("apply: a successful apply writes and verifies the active state") {
    InMemoryNodeBackend backend;
    backend.seed(governor_node().path, "schedutil");
    CapabilityRegistry reg;
    reg.register_node(governor_node(), backend);
    ExecutionEngine engine(backend);

    ApplyResult r = engine.apply(perf_plan(reg, backend), "performance", "balanced", "game", 1000);
    CHECK(r.verified_active);
    CHECK(!r.critical_failure);
    CHECK_EQ(r.succeeded, 1);
    CHECK_EQ(backend.read(governor_node().path).value(), std::string("performance"));
}

TEST("apply: repeating the same decision does not rewrite (idempotent)") {
    InMemoryNodeBackend backend;
    backend.seed(governor_node().path, "schedutil");
    CapabilityRegistry reg;
    reg.register_node(governor_node(), backend);
    ExecutionEngine engine(backend);

    engine.apply(perf_plan(reg, backend), "performance", "balanced", "game", 1000);
    CHECK_EQ(backend.write_count(governor_node().path), 1);

    ApplyResult again = engine.apply(perf_plan(reg, backend), "performance", "performance", "game", 2000);
    CHECK_EQ(again.skipped_idempotent, 1);
    CHECK_EQ(again.succeeded, 0);
    CHECK_EQ(backend.write_count(governor_node().path), 1); // no second write
}

TEST("apply: invalidation forces a rewrite (daemon restart / external mutation)") {
    InMemoryNodeBackend backend;
    backend.seed(governor_node().path, "schedutil");
    CapabilityRegistry reg;
    reg.register_node(governor_node(), backend);
    ExecutionEngine engine(backend);

    engine.apply(perf_plan(reg, backend), "performance", "balanced", "game", 1000);
    engine.invalidate_all(); // e.g. after a restart, or external mutation detected
    ApplyResult again = engine.apply(perf_plan(reg, backend), "performance", "performance", "game", 2000);
    CHECK_EQ(again.succeeded, 1);
    CHECK_EQ(backend.write_count(governor_node().path), 2);
}

TEST("apply: a read-back mismatch on a critical node is a critical failure with rollback") {
    InMemoryNodeBackend backend;
    backend.seed(governor_node().path, "schedutil");
    backend.ignore_value(governor_node().path, "performance"); // node silently ignores this value
    CapabilityRegistry reg;
    reg.register_node(governor_node(), backend);
    ExecutionEngine engine(backend);

    ApplyResult r = engine.apply(perf_plan(reg, backend), "performance", "balanced", "game", 1000);
    CHECK(r.critical_failure);
    CHECK(!r.verified_active);
    CHECK(r.rollback_attempted);
    CHECK(r.rollback_succeeded);
    CHECK_EQ(backend.read(governor_node().path).value(), std::string("schedutil")); // restored
}

TEST("apply: a critical write failure rolls back the critical group") {
    InMemoryNodeBackend backend;
    backend.seed(governor_node().path, "schedutil");
    backend.fail_writes_to(governor_node().path);
    CapabilityRegistry reg;
    reg.register_node(governor_node(), backend);
    ExecutionEngine engine(backend);

    ApplyResult r = engine.apply(perf_plan(reg, backend), "performance", "balanced", "game", 1000);
    CHECK(r.critical_failure);
    CHECK_EQ(r.succeeded, 0);
    CHECK(!r.verified_active);
}

TEST("apply: an optional write failure is recorded but not fatal") {
    InMemoryNodeBackend backend;
    backend.seed(governor_node().path, "schedutil");
    backend.seed(freq_node().path, "1000000");
    backend.fail_writes_to(freq_node().path); // freq is non-critical
    CapabilityRegistry reg;
    reg.register_node(governor_node(), backend);
    reg.register_node(freq_node(), backend);
    ExecutionEngine engine(backend);

    ExecutionPlan p = ExecutionPlanner{}.plan(
        spec({{"cpu.policy0.governor", "performance", "game"},
              {"cpu.policy0.max_freq", "1800000", "game"}}),
        reg, backend);
    ApplyResult r = engine.apply(p, "performance", "balanced", "game", 1000);
    CHECK(!r.critical_failure);
    CHECK(r.verified_active);
    CHECK_EQ(r.optional_failures, 1);
    CHECK_EQ(r.succeeded, 1); // the governor
}

TEST("apply: rollback that cannot restore a previous value reports degraded") {
    InMemoryNodeBackend backend;
    // A critical node with no readable previous value, that clamps -> verify fails, and there
    // is nothing safe to roll back to.
    NodeDescriptor gov = governor_node();
    gov.readable = false; // previous value cannot be captured
    backend.seed(gov.path, "schedutil");
    backend.ignore_value(gov.path, "performance");
    CapabilityRegistry reg;
    reg.register_node(gov, backend);
    ExecutionEngine engine(backend);

    ApplyResult r = engine.apply(
        ExecutionPlanner{}.plan(spec({{"cpu.policy0.governor", "performance", "game"}}), reg, backend),
        "performance", "balanced", "game", 1000);
    CHECK(r.critical_failure);
    CHECK(r.rollback_attempted);
    CHECK(!r.rollback_succeeded);
    CHECK(r.degraded);
    CHECK_EQ(r.degraded_capability, std::string("cpu.policy0.governor"));
}

// --- Restoration & history -------------------------------------------------

TEST("restore: original values are captured on first apply and restored on demand") {
    InMemoryNodeBackend backend;
    backend.seed(governor_node().path, "schedutil"); // the user's original
    CapabilityRegistry reg;
    reg.register_node(governor_node(), backend);
    ExecutionEngine engine(backend);

    engine.apply(perf_plan(reg, backend), "performance", "balanced", "game", 1000);
    CHECK_EQ(backend.read(governor_node().path).value(), std::string("performance"));

    ApplyResult restore = engine.restore_originals("session_ended", 2000);
    CHECK(restore.verified_active);
    CHECK_EQ(backend.read(governor_node().path).value(), std::string("schedutil")); // original back
}

TEST("history: apply history is bounded to its capacity") {
    InMemoryNodeBackend backend;
    backend.seed(governor_node().path, "schedutil");
    CapabilityRegistry reg;
    reg.register_node(governor_node(), backend);
    ExecutionEngine engine(backend, /*history_capacity=*/4);

    for (int i = 0; i < 10; ++i) {
        engine.invalidate_all();
        engine.apply(perf_plan(reg, backend), "performance", "balanced", "game", 1000 + i);
    }
    CHECK_EQ(engine.history().size(), 4u);
}

TEST("apply: an invalid plan is refused without any writes") {
    InMemoryNodeBackend backend;
    CapabilityRegistry reg;
    ExecutionEngine engine(backend);

    ExecutionPlan bad;
    bad.valid = false;
    bad.invalid_reason = "synthetic";
    ApplyResult r = engine.apply(bad, "performance", "balanced", "x", 1000);
    CHECK(r.critical_failure);
    CHECK(!r.verified_active);
    CHECK_EQ(r.succeeded, 0);
}
