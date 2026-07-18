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

// PolicyDecision -> DryRunExecutionPlan, against fake device trees.
//
// Every test asserts the plan changed nothing: no value written, no mode altered, and the last
// verified live profile untouched. A dry run that mutated anything would defeat its purpose.

#include "TestFramework.hpp"

#include "DevicePacks.hpp"
#include "DryRunPlanner.hpp"
#include "PolicyIntent.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace flux::execution;
using flux::engine::DataHealth;
using flux::engine::Decision;
using flux::engine::DecisionPriority;
using flux::engine::DecisionReason;
using flux::engine::TargetProfile;

namespace {

class FakeTree {
public:
    FakeTree() {
        char tmpl[] = "/tmp/flux_dryrun_XXXXXX";
        const char *dir = mkdtemp(tmpl);
        root_ = dir ? dir : "";
    }
    ~FakeTree() {
        if (!root_.empty()) {
            const std::string cmd = "rm -rf '" + root_ + "'";
            if (system(cmd.c_str()) != 0) { /* best effort */
            }
        }
    }
    FakeTree(const FakeTree &) = delete;
    FakeTree &operator=(const FakeTree &) = delete;

    void node(const std::string &relative, const std::string &content, mode_t mode = 0644) {
        const std::string full = root_ + relative;
        size_t pos = full.find('/', 1);
        while (pos != std::string::npos) {
            mkdir(full.substr(0, pos).c_str(), 0755);
            pos = full.find('/', pos + 1);
        }
        std::ofstream out(full);
        out << content;
        out.close();
        chmod(full.c_str(), mode);
        tracked_.push_back(full);
    }

    [[nodiscard]] std::string rebase(const std::string &absolute) const { return root_ + absolute; }
    [[nodiscard]] PathPolicy policy() const {
        return PathPolicy(std::vector<std::string>{root_ + "/"});
    }

    [[nodiscard]] std::vector<std::string> snapshot() const {
        std::vector<std::string> out;
        for (const auto &p : tracked_) {
            struct stat st {};
            if (lstat(p.c_str(), &st) != 0) continue;
            std::ifstream in(p);
            std::string content((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            out.push_back(p + "|" + content + "|" + std::to_string(st.st_mode & 07777));
        }
        return out;
    }

private:
    std::string root_;
    std::vector<std::string> tracked_;
};

DevicePack rebase_pack(DevicePack pack, const FakeTree &tree) {
    for (auto &d : pack.descriptors) d.path = tree.rebase(d.path);
    return pack;
}

/// Promote a pack's descriptors as if hardware validation had happened, to test the planner's
/// logic rather than the (already-tested) validation gate.
DevicePack as_validated(DevicePack pack) {
    for (auto &d : pack.descriptors) d.validation = ValidationStatus::PhysicalDeviceValidated;
    return pack;
}

Decision decide(TargetProfile profile, DecisionReason reason, DecisionPriority priority,
                DataHealth health = DataHealth::Healthy) {
    Decision d;
    d.desired_profile = profile;
    d.reason = reason;
    d.priority = priority;
    d.health = health;
    return d;
}

/// A generic tree with the cpufreq nodes the generic pack expects.
void seed_generic(FakeTree &tree) {
    for (int policy : {0, 4, 7}) {
        tree.node("/sys/devices/system/cpu/cpufreq/policy" + std::to_string(policy) +
                      "/scaling_governor",
                  "schedutil", 0644);
    }
}

/// A descriptor serving exactly one group with one intent key, on a node seeded in @p tree.
CapabilityDescriptor token_descriptor(const std::string &id, CapabilityGroup group,
                                      const std::string &path, const std::string &key,
                                      const std::string &value) {
    CapabilityDescriptor d;
    d.descriptor_id = id;
    d.capability_id = id;
    d.group = group;
    d.soc = SocFamily::Generic;
    d.path = path;
    d.value_type = NodeValueType::Token;
    d.allowed = {value, "idle"};
    d.policy_values = {{key, value}};
    d.validation = ValidationStatus::PhysicalDeviceValidated;
    return d;
}

/// A pack that answers every intent a sustained-performance ask expands to. Nothing in the real
/// device tables is this complete — that is the point: it pins down what "fully active" means, so
/// the flag is proven reachable rather than dead.
DevicePack complete_pack(FakeTree &tree) {
    DevicePack pack;
    pack.schema_version = kDescriptorSchemaVersion;
    pack.pack_id = "complete";
    pack.soc = SocFamily::Generic;
    pack.provenance = Provenance::FluxAuthored;

    const struct {
        const char *id;
        CapabilityGroup group;
        const char *node;
    } wanted[] = {
        {"t.cpu", CapabilityGroup::CpuPolicy, "/sys/flux_test/cpu"},
        {"t.gpu", CapabilityGroup::GpuPolicy, "/sys/flux_test/gpu"},
        {"t.memory", CapabilityGroup::Memory, "/sys/flux_test/memory"},
        {"t.scheduler", CapabilityGroup::Scheduler, "/sys/flux_test/scheduler"},
        {"t.vendor", CapabilityGroup::DeviceSpecific, "/sys/flux_test/vendor"},
    };
    for (const auto &w : wanted) {
        tree.node(w.node, "idle", 0644);
        pack.descriptors.push_back(token_descriptor(w.id, w.group, tree.rebase(w.node),
                                                    "sustained_performance", "on"));
    }
    return pack;
}

} // namespace

TEST("dry run plan: fully active requires every intent served, including the optional ones") {
    // The positive case for would_be_fully_active. Its absence would let the flag be quietly
    // unreachable, and a claim that can never be made is indistinguishable from a broken one.
    FakeTree tree;
    SysfsNodeBackend backend(tree.policy());
    const CapabilityProbe probe(backend, tree.policy());
    const DryRunPlanner planner(probe);

    const auto intents = IntentMapper::from_decision(
        decide(TargetProfile::Performance, DecisionReason::session_started,
               DecisionPriority::SessionLifecycle));
    const std::vector<DevicePack> packs = {complete_pack(tree)};
    const auto before = tree.snapshot();

    const auto plan =
        planner.plan(intents, packs, {SocFamily::Generic, "fake"}, TargetProfile::Balanced, 1000);

    CHECK_MSG(plan.valid, "expected a valid plan, got: " + plan.invalid_reason);
    CHECK_MSG(plan.projected.would_be_fully_active,
              "every intent is served by a validated descriptor, so the plan may say so; "
              "readiness was " +
                  std::string(execution_readiness_name(plan.projected.readiness)) + ", " +
                  std::to_string(plan.optional_skips) + " optional skips, " +
                  std::to_string(plan.prevented_count()) + " prevented");
    CHECK_EQ(plan.executable_count(), static_cast<size_t>(5));
    CHECK_MSG(tree.snapshot() == before, "even a fully active projection writes nothing");
}

TEST("dry run plan: one unserved optional intent withdraws the fully-active claim") {
    // Same fixture minus the GPU descriptor. Nothing fails, the plan stays valid and the four
    // remaining actions still run — but the ask was not fully met, so it must not say it was.
    FakeTree tree;
    SysfsNodeBackend backend(tree.policy());
    const CapabilityProbe probe(backend, tree.policy());
    const DryRunPlanner planner(probe);

    auto pack = complete_pack(tree);
    pack.descriptors.erase(std::remove_if(pack.descriptors.begin(), pack.descriptors.end(),
                                          [](const CapabilityDescriptor &d) {
                                              return d.group == CapabilityGroup::GpuPolicy;
                                          }),
                           pack.descriptors.end());

    const auto intents = IntentMapper::from_decision(
        decide(TargetProfile::Performance, DecisionReason::session_started,
               DecisionPriority::SessionLifecycle));

    const auto plan = planner.plan(intents, {pack}, {SocFamily::Generic, "fake"},
                                   TargetProfile::Balanced, 1000);

    CHECK_MSG(plan.valid, "an unserved optional intent is not a plan failure");
    CHECK_EQ(plan.executable_count(), static_cast<size_t>(4));
    CHECK_MSG(!plan.critical_rejection, "the GPU is optional; nothing may be withdrawn");
    CHECK_MSG(!plan.projected.would_be_fully_active,
              "no GPU descriptor served the ask, so the profile is not fully active");
}

TEST("dry run plan: a plan performs zero writes and leaves every mode alone") {
    FakeTree tree;
    seed_generic(tree);
    tree.node("/sys/class/kgsl/kgsl-3d0/force_clk_on", "0", 0444);
    const auto before = tree.snapshot();

    SysfsNodeBackend backend(tree.policy());
    const CapabilityProbe probe(backend, tree.policy());
    const DryRunPlanner planner(probe);

    const auto intents = IntentMapper::from_decision(
        decide(TargetProfile::Performance, DecisionReason::session_started,
               DecisionPriority::SessionLifecycle));
    const std::vector<DevicePack> packs = {
        as_validated(rebase_pack(flux::device::generic_pack(), tree)),
        as_validated(rebase_pack(flux::device::snapdragon_pack(), tree))};

    (void)planner.plan(intents, packs, {SocFamily::Snapdragon, "fake"}, TargetProfile::Balanced,
                       1000);

    CHECK_MSG(tree.snapshot() == before, "planning must not write a value or change a mode");
}

TEST("dry run plan: a dry run never advances the last verified live profile") {
    // The single most important thing a dry run must not do.
    FakeTree tree;
    seed_generic(tree);

    SysfsNodeBackend backend(tree.policy());
    const CapabilityProbe probe(backend, tree.policy());
    const DryRunPlanner planner(probe);

    const auto intents = IntentMapper::from_decision(
        decide(TargetProfile::Performance, DecisionReason::session_started,
               DecisionPriority::SessionLifecycle));
    const std::vector<DevicePack> packs = {
        as_validated(rebase_pack(flux::device::generic_pack(), tree))};

    const auto plan = planner.plan(intents, packs, {SocFamily::Generic, "fake"},
                                   TargetProfile::PowerSave, 1000);

    CHECK_MSG(plan.projected.last_verified_profile == TargetProfile::PowerSave,
              "the last verified profile must survive a dry run untouched");
    CHECK_EQ(plan.projected.requested_profile, TargetProfile::Performance);
    CHECK_MSG(!plan.projected.would_be_fully_active || plan.valid,
              "a plan cannot claim it would be active unless it is valid");
}

TEST("dry run plan: a validated generic device produces a complete valid plan") {
    FakeTree tree;
    seed_generic(tree);

    SysfsNodeBackend backend(tree.policy());
    const CapabilityProbe probe(backend, tree.policy());
    const DryRunPlanner planner(probe);

    const auto intents = IntentMapper::from_decision(
        decide(TargetProfile::Performance, DecisionReason::session_started,
               DecisionPriority::SessionLifecycle));
    const std::vector<DevicePack> packs = {
        as_validated(rebase_pack(flux::device::generic_pack(), tree))};

    const auto plan = planner.plan(intents, packs, {SocFamily::Generic, "fake"},
                                   TargetProfile::Balanced, 1000);

    CHECK_MSG(plan.valid, "expected a valid plan, got: " + plan.invalid_reason);
    CHECK_MSG(plan.executable_count() == 3, "all three cpufreq policies should be planned");
    CHECK(!plan.critical_rejection);

    for (const auto &action : plan.actions) {
        CHECK_EQ(action.desired_value, std::string("performance"));
        CHECK_EQ(action.backend, std::string("sysfs"));
        CHECK_EQ(action.capability_state, CapabilityState::Supported);
        CHECK_MSG(!action.target_path.empty(), "an action must name its validated target");
        CHECK_MSG(!action.action_id.empty(), "an action must be identifiable");
    }
}

TEST("dry run plan: unvalidated vendor descriptors produce no actions") {
    // The shipped packs are PhysicalDeviceRequired, so even a device with every vendor node
    // present must plan nothing from them.
    FakeTree tree;
    tree.node("/sys/class/kgsl/kgsl-3d0/force_clk_on", "0", 0644);
    tree.node("/sys/class/kgsl/kgsl-3d0/bus_split", "1", 0644);

    SysfsNodeBackend backend(tree.policy());
    const CapabilityProbe probe(backend, tree.policy());
    const DryRunPlanner planner(probe);

    const auto intents = IntentMapper::from_decision(
        decide(TargetProfile::Performance, DecisionReason::session_started,
               DecisionPriority::SessionLifecycle));
    // NOT promoted: exactly as shipped.
    const std::vector<DevicePack> packs = {rebase_pack(flux::device::snapdragon_pack(), tree)};

    const auto plan = planner.plan(intents, packs, {SocFamily::Snapdragon, "fake"},
                                   TargetProfile::Balanced, 1000);

    CHECK_MSG(plan.executable_count() == 0,
              "no unvalidated vendor descriptor may produce an executable action");
    CHECK(plan.projected.device_validation_pending);
    CHECK_MSG(!plan.projected.would_be_fully_active,
              "a profile must never be reported fully active on unvalidated capabilities");

    const bool traced = std::any_of(plan.prevented.begin(), plan.prevented.end(),
                                    [](const PreventedAction &p) {
                                        return p.state == CapabilityState::DeviceValidationRequired;
                                    });
    CHECK_MSG(traced, "the prevention must be explained as awaiting device validation");
}

TEST("dry run plan: vendor unavailable plus generic supported is capability_limited") {
    // The common real case, and the fallback the spec names explicitly: the ask is partly met,
    // and the plan says so rather than claiming the profile worked.
    FakeTree tree;
    seed_generic(tree);
    tree.node("/sys/class/kgsl/kgsl-3d0/force_clk_on", "0", 0644);

    SysfsNodeBackend backend(tree.policy());
    const CapabilityProbe probe(backend, tree.policy());
    const DryRunPlanner planner(probe);

    const auto intents = IntentMapper::from_decision(
        decide(TargetProfile::Performance, DecisionReason::session_started,
               DecisionPriority::SessionLifecycle));
    const std::vector<DevicePack> packs = {
        as_validated(rebase_pack(flux::device::generic_pack(), tree)),
        rebase_pack(flux::device::snapdragon_pack(), tree)}; // vendor left unvalidated

    const auto plan = planner.plan(intents, packs, {SocFamily::Snapdragon, "fake"},
                                   TargetProfile::Balanced, 1000);

    CHECK_MSG(plan.executable_count() > 0, "generic capabilities must still be planned");
    CHECK_EQ(plan.projected.readiness, ExecutionReadiness::CapabilityLimited);
    CHECK_MSG(!plan.projected.would_be_fully_active,
              "partly-met asks are not fully active");
    CHECK_MSG(!plan.projected.unsupported_reason.empty(), "the shortfall must be explained");
}

TEST("dry run plan: no capability at all yields a conservative no-op plan") {
    FakeTree tree;
    tree.node("/sys/placeholder", "1", 0644);

    SysfsNodeBackend backend(tree.policy());
    const CapabilityProbe probe(backend, tree.policy());
    const DryRunPlanner planner(probe);

    const auto intents = IntentMapper::from_decision(
        decide(TargetProfile::Performance, DecisionReason::session_started,
               DecisionPriority::SessionLifecycle));
    const std::vector<DevicePack> packs = {
        as_validated(rebase_pack(flux::device::generic_pack(), tree))};

    const auto plan = planner.plan(intents, packs, {SocFamily::Generic, "bare"},
                                   TargetProfile::Balanced, 1000);

    CHECK_EQ(plan.executable_count(), static_cast<size_t>(0));
    CHECK_MSG(!plan.projected.would_be_fully_active, "nothing was planned; nothing is active");
    CHECK_MSG(plan.projected.readiness == ExecutionReadiness::Unsupported ||
                  plan.projected.readiness == ExecutionReadiness::CapabilityLimited,
              "a bare device must report unsupported or capability-limited, got: " +
                  std::string(execution_readiness_name(plan.projected.readiness)));
}

TEST("dry run plan: an aggressive plan is withdrawn whole when a critical capability fails") {
    // Half of an aggressive tuning set leaves the device in a combination nobody designed.
    FakeTree tree;
    // GPU present, but no cpufreq at all -> the critical cpu intent cannot be served.
    tree.node("/sys/class/kgsl/kgsl-3d0/force_clk_on", "0", 0644);

    SysfsNodeBackend backend(tree.policy());
    const CapabilityProbe probe(backend, tree.policy());
    const DryRunPlanner planner(probe);

    const auto intents = IntentMapper::from_decision(
        decide(TargetProfile::Performance, DecisionReason::session_started,
               DecisionPriority::SessionLifecycle));
    const std::vector<DevicePack> packs = {
        as_validated(rebase_pack(flux::device::generic_pack(), tree)),
        as_validated(rebase_pack(flux::device::snapdragon_pack(), tree))};

    const auto plan = planner.plan(intents, packs, {SocFamily::Snapdragon, "fake"},
                                   TargetProfile::Balanced, 1000);

    CHECK_MSG(plan.critical_rejection, "a missing critical capability must be recorded");
    CHECK_MSG(plan.executable_count() == 0,
              "the aggressive plan must be withdrawn whole, not applied partially");
    CHECK_EQ(plan.projected.readiness, ExecutionReadiness::CapabilityLimited);
    CHECK(!plan.projected.would_be_fully_active);
}

TEST("dry run plan: a safety downgrade is not blocked by a missing performance capability") {
    // Safety must proceed independently of performance support: the opposite would mean a
    // device that cannot go fast also cannot be made safe.
    FakeTree tree;
    seed_generic(tree);

    SysfsNodeBackend backend(tree.policy());
    const CapabilityProbe probe(backend, tree.policy());
    const DryRunPlanner planner(probe);

    auto d = decide(TargetProfile::Balanced, DecisionReason::thermal_emergency,
                    DecisionPriority::ThermalEmergency);
    d.safety_driven = true;
    const auto intents = IntentMapper::from_decision(d);
    const std::vector<DevicePack> packs = {
        as_validated(rebase_pack(flux::device::generic_pack(), tree))};

    const auto plan = planner.plan(intents, packs, {SocFamily::Generic, "fake"},
                                   TargetProfile::Performance, 1000);

    CHECK_MSG(plan.executable_count() > 0, "a thermal downgrade must still be planned");
    CHECK_MSG(!plan.critical_rejection, "a safe ask has no critical performance capability to lose");
    for (const auto &action : plan.actions) {
        CHECK_EQ(action.desired_value, std::string("schedutil"));
    }
}

TEST("dry run plan: actions are ordered deterministically") {
    FakeTree tree;
    seed_generic(tree);

    SysfsNodeBackend backend(tree.policy());
    const CapabilityProbe probe(backend, tree.policy());
    const DryRunPlanner planner(probe);

    const auto intents = IntentMapper::from_decision(
        decide(TargetProfile::Performance, DecisionReason::session_started,
               DecisionPriority::SessionLifecycle));
    const std::vector<DevicePack> packs = {
        as_validated(rebase_pack(flux::device::generic_pack(), tree))};

    const auto a = planner.plan(intents, packs, {SocFamily::Generic, "f"}, TargetProfile::Balanced, 1);
    const auto b = planner.plan(intents, packs, {SocFamily::Generic, "f"}, TargetProfile::Balanced, 1);

    CHECK_EQ(a.actions.size(), b.actions.size());
    for (size_t i = 0; i < a.actions.size(); ++i) {
        CHECK_EQ(a.actions[i].action_id, b.actions[i].action_id);
    }
    for (size_t i = 1; i < a.actions.size(); ++i) {
        const bool ordered = a.actions[i - 1].order_group < a.actions[i].order_group ||
                             (a.actions[i - 1].order_group == a.actions[i].order_group &&
                              a.actions[i - 1].action_id <= a.actions[i].action_id);
        CHECK_MSG(ordered, "actions must be ordered by (order_group, action_id)");
    }
}

TEST("dry run plan: stale telemetry marks the plan degraded") {
    FakeTree tree;
    seed_generic(tree);

    SysfsNodeBackend backend(tree.policy());
    const CapabilityProbe probe(backend, tree.policy());
    const DryRunPlanner planner(probe);

    auto d = decide(TargetProfile::Balanced, DecisionReason::telemetry_stale,
                    DecisionPriority::TelemetrySafety, DataHealth::Stale);
    d.constraints.promotion_locked = true;
    const auto intents = IntentMapper::from_decision(d);
    const std::vector<DevicePack> packs = {
        as_validated(rebase_pack(flux::device::generic_pack(), tree))};

    const auto plan = planner.plan(intents, packs, {SocFamily::Generic, "f"},
                                   TargetProfile::Performance, 1000);

    CHECK_EQ(plan.projected.readiness, ExecutionReadiness::TelemetryDegraded);
    CHECK_MSG(!plan.projected.would_be_fully_active,
              "a plan built on stale data must not claim it would be fully active");
    CHECK_EQ(plan.projected.telemetry_health, DataHealth::Stale);
}

TEST("dry run plan: the plan records rollback coverage honestly") {
    FakeTree tree;
    seed_generic(tree);

    SysfsNodeBackend backend(tree.policy());
    const CapabilityProbe probe(backend, tree.policy());
    const DryRunPlanner planner(probe);

    const auto intents = IntentMapper::from_decision(
        decide(TargetProfile::Performance, DecisionReason::session_started,
               DecisionPriority::SessionLifecycle));
    const std::vector<DevicePack> packs = {
        as_validated(rebase_pack(flux::device::generic_pack(), tree))};

    const auto plan = planner.plan(intents, packs, {SocFamily::Generic, "f"},
                                   TargetProfile::Balanced, 1000);

    CHECK_EQ(plan.rollback_covered, static_cast<int>(plan.actions.size()));
    CHECK_MSG(plan.projected.rollback_fully_covered,
              "readable cpufreq nodes can all give their original value back");
    for (const auto &action : plan.actions) {
        CHECK_EQ(action.rollback, RollbackStrategy::RestoreOriginal);
        CHECK(action.rollback_value_available);
    }
}

TEST("dry run plan: every prevented action explains itself") {
    FakeTree tree;
    tree.node("/sys/class/kgsl/kgsl-3d0/force_clk_on", "0", 0644);

    SysfsNodeBackend backend(tree.policy());
    const CapabilityProbe probe(backend, tree.policy());
    const DryRunPlanner planner(probe);

    const auto intents = IntentMapper::from_decision(
        decide(TargetProfile::Performance, DecisionReason::session_started,
               DecisionPriority::SessionLifecycle));
    const std::vector<DevicePack> packs = {rebase_pack(flux::device::snapdragon_pack(), tree)};

    const auto plan = planner.plan(intents, packs, {SocFamily::Snapdragon, "f"},
                                   TargetProfile::Balanced, 1000);

    CHECK_MSG(!plan.prevented.empty(), "something must have been prevented here");
    for (const auto &prevented : plan.prevented) {
        CHECK_MSG(!prevented.capability_id.empty(), "a prevention must name its capability");
        CHECK_MSG(!prevented.descriptor_set.empty(), "a prevention must name its pack");
        CHECK_MSG(!prevented.detail.empty(), "a prevention must explain itself");
        CHECK_MSG(prevented.state != CapabilityState::Supported,
                  "a supported capability cannot be prevented");
    }
    CHECK_MSG(!plan.trace.empty(), "the plan must carry a resolution trace");
}

TEST("dry run plan: all six SoC families plus generic plan without writing") {
    // The full matrix. Every family is gated, so the meaningful assertions are that nothing is
    // written, nothing is claimed active, and every family is reachable.
    struct Family {
        const char *name;
        SocFamily soc;
        DevicePack (*pack)();
    };
    const Family families[] = {
        {"mediatek", SocFamily::MediaTek, flux::device::mediatek_pack},
        {"snapdragon", SocFamily::Snapdragon, flux::device::snapdragon_pack},
        {"exynos", SocFamily::Exynos, flux::device::exynos_pack},
        {"unisoc", SocFamily::Unisoc, flux::device::unisoc_pack},
        {"tensor", SocFamily::Tensor, flux::device::tensor_pack},
        {"tegra", SocFamily::Tegra, flux::device::tegra_pack},
    };

    for (const auto &family : families) {
        FakeTree tree;
        seed_generic(tree);
        const auto before = tree.snapshot();

        SysfsNodeBackend backend(tree.policy());
        const CapabilityProbe probe(backend, tree.policy());
        const DryRunPlanner planner(probe);

        const auto intents = IntentMapper::from_decision(
            decide(TargetProfile::Performance, DecisionReason::session_started,
                   DecisionPriority::SessionLifecycle));
        // Generic validated (upstream Linux, understood); vendor exactly as shipped.
        const std::vector<DevicePack> packs = {
            as_validated(rebase_pack(flux::device::generic_pack(), tree)),
            rebase_pack(family.pack(), tree)};

        const auto plan = planner.plan(intents, packs, {family.soc, "fake"},
                                       TargetProfile::Balanced, 1000);

        CHECK_MSG(tree.snapshot() == before,
                  std::string(family.name) + ": planning must write nothing");
        CHECK_MSG(!plan.projected.would_be_fully_active,
                  std::string(family.name) +
                      ": no family may be reported fully active while gated");
        CHECK_MSG(plan.projected.last_verified_profile == TargetProfile::Balanced,
                  std::string(family.name) + ": the verified profile must be untouched");
        // The generic fallback still works underneath every family.
        CHECK_MSG(plan.executable_count() == 3,
                  std::string(family.name) +
                      ": the generic fallback should still plan its three cpufreq actions, got " +
                      std::to_string(plan.executable_count()));
    }
}

TEST("dry run plan: a wrong-SoC pack contributes nothing") {
    FakeTree tree;
    seed_generic(tree);
    tree.node("/sys/class/kgsl/kgsl-3d0/force_clk_on", "0", 0644);

    SysfsNodeBackend backend(tree.policy());
    const CapabilityProbe probe(backend, tree.policy());
    const DryRunPlanner planner(probe);

    const auto intents = IntentMapper::from_decision(
        decide(TargetProfile::Performance, DecisionReason::session_started,
               DecisionPriority::SessionLifecycle));
    // Snapdragon pack, validated, but the device says MediaTek.
    const std::vector<DevicePack> packs = {
        as_validated(rebase_pack(flux::device::generic_pack(), tree)),
        as_validated(rebase_pack(flux::device::snapdragon_pack(), tree))};

    const auto plan = planner.plan(intents, packs, {SocFamily::MediaTek, "fake"},
                                   TargetProfile::Balanced, 1000);

    const bool any_snapdragon = std::any_of(plan.actions.begin(), plan.actions.end(),
                                            [](const DryRunAction &a) {
                                                return a.descriptor_set == "snapdragon";
                                            });
    CHECK_MSG(!any_snapdragon, "a pack for another SoC must never contribute an action");
}
