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

// The live composition root: ExecutionRuntime + RuntimeProfileState + zen.
//
// These test the cutover's actual promises — one write path, verified-only profile advancement,
// gated vendor capabilities, coalescing that never delays a safety downgrade, and a runtime that
// stops writing once shut down — against a real temp filesystem tree.

#include "TestFramework.hpp"

#include "DevicePacks.hpp"
#include "ExecutionRuntime.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace flux::execution;
namespace eng = flux::engine;

namespace {

/// A real tree with the generic pack's cpufreq nodes.
class TempTree {
public:
    TempTree() {
        char tmpl[] = "/tmp/flux_runtime_root_XXXXXX";
        const char *dir = mkdtemp(tmpl);
        root_ = dir ? dir : "";
        for (int policy : {0, 4, 7}) {
            write_node("/sys/devices/system/cpu/cpufreq/policy" + std::to_string(policy) +
                           "/scaling_governor",
                       "schedutil");
        }
    }
    ~TempTree() {
        if (!root_.empty()) std::filesystem::remove_all(root_);
    }
    TempTree(const TempTree &) = delete;
    TempTree &operator=(const TempTree &) = delete;

    void write_node(const std::string &relative, const std::string &content) {
        const std::string full = root_ + relative;
        std::filesystem::create_directories(std::filesystem::path(full).parent_path());
        std::ofstream out(full);
        out << content;
    }
    [[nodiscard]] std::string read_node(const std::string &relative) const {
        std::ifstream in(root_ + relative);
        std::string value;
        std::getline(in, value);
        return value;
    }
    [[nodiscard]] std::string rebase(const std::string &absolute) const { return root_ + absolute; }
    [[nodiscard]] PathPolicy policy() const {
        return PathPolicy(std::vector<std::string>{root_ + "/"});
    }
    [[nodiscard]] std::string governor0() const {
        return read_node("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor");
    }

private:
    std::string root_;
};

DevicePack generic_on(const TempTree &tree, bool validated = true) {
    DevicePack pack = flux::device::generic_pack();
    for (auto &d : pack.descriptors) {
        d.path = tree.rebase(d.path);
        if (validated) d.validation = ValidationStatus::PhysicalDeviceValidated;
    }
    return pack;
}

eng::Decision decide(eng::TargetProfile profile, eng::DecisionReason reason,
                     eng::DecisionPriority priority,
                     eng::DataHealth health = eng::DataHealth::Healthy) {
    eng::Decision d;
    d.desired_profile = profile;
    d.reason = reason;
    d.priority = priority;
    d.health = health;
    return d;
}

eng::Decision performance_decision() {
    return decide(eng::TargetProfile::Performance, eng::DecisionReason::session_started,
                  eng::DecisionPriority::SessionLifecycle);
}

/// A zen backend that records every call, so "exactly one zen write path" is checkable.
class FakeZen : public ZenBackend {
public:
    explicit FakeZen(std::optional<int> initial = 0) : mode_(initial) {}

    [[nodiscard]] bool available() const override { return mode_.has_value(); }
    [[nodiscard]] std::optional<int> read() const override { return mode_; }
    bool set(int mode) override {
        if (!mode_) return false;
        ++writes;
        applied.push_back(mode);
        mode_ = mode;
        return true;
    }
    void user_changes_to(int mode) { mode_ = mode; }
    void make_unavailable() { mode_ = std::nullopt; }

    int writes = 0;
    std::vector<int> applied;

private:
    std::optional<int> mode_;
};

SessionContext game_session(bool wants_zen = false, int zen_mode = 1) {
    SessionContext s;
    s.in_session = true;
    s.package = "com.example.game";
    s.pid = 1234;
    s.wants_zen = wants_zen;
    s.desired_zen_mode = zen_mode;
    return s;
}

} // namespace

// --- composition root ------------------------------------------------------

TEST("runtime: a decision reaches the device through the engine, and verifies") {
    TempTree tree;
    ExecutionRuntime runtime({generic_on(tree)}, {SocFamily::Generic, "fake"}, tree.policy());

    const auto cycle = runtime.on_decision(performance_decision(), game_session(), 1000);

    CHECK_MSG(cycle.planned, "a decision must produce a plan");
    CHECK_MSG(cycle.applied, "a plan must reach the engine");
    CHECK_MSG(cycle.apply.verified_active, "the apply must verify: " + cycle.apply.message);
    CHECK_EQ(tree.governor0(), std::string("performance"));
    CHECK_EQ(runtime.state().verified_profile(), eng::TargetProfile::Performance);
    CHECK_EQ(runtime.state().state(), ApplyState::Verified);
}

TEST("runtime: shutdown stops every write, and is idempotent") {
    TempTree tree;
    ExecutionRuntime runtime({generic_on(tree)}, {SocFamily::Generic, "fake"}, tree.policy());

    runtime.shutdown();
    runtime.shutdown(); // idempotent

    const auto cycle = runtime.on_decision(performance_decision(), game_session(), 1000);

    CHECK(runtime.is_shut_down());
    CHECK_MSG(!cycle.applied, "no cycle may run once shutdown has begun");
    CHECK_MSG(tree.governor0() == "schedutil", "a shut-down runtime must not touch the device");
    CHECK_MSG(!runtime.restore_all("late", 2000).verified_active,
              "even a restore must not write after shutdown");
}

TEST("runtime: history is bounded and owned by the one engine") {
    TempTree tree;
    ExecutionRuntime runtime({generic_on(tree)}, {SocFamily::Generic, "fake"}, tree.policy(),
                             nullptr, /*history_capacity=*/2);

    for (int i = 0; i < 5; ++i) {
        runtime.invalidate_capabilities("test");
        (void)runtime.on_decision(performance_decision(), game_session(), 1000 + i);
    }

    CHECK_EQ(runtime.history().size(), static_cast<size_t>(2));
}

// --- the verified-profile rule ---------------------------------------------

TEST("runtime: a decision alone never advances the verified profile") {
    // The rule the whole increment exists for. The device has no writable node at all, so a
    // decision is made, a plan is built, and nothing is verified.
    TempTree tree;
    auto pack = generic_on(tree);
    for (auto &d : pack.descriptors) d.path = tree.rebase("/sys/does/not/exist");

    ExecutionRuntime runtime({pack}, {SocFamily::Generic, "fake"}, tree.policy());
    const auto cycle = runtime.on_decision(performance_decision(), game_session(), 1000);

    CHECK_MSG(!cycle.apply.verified_active, "nothing was written, so nothing is verified");
    CHECK_MSG(runtime.state().verified_profile() != eng::TargetProfile::Performance,
              "a profile that was only asked for is not the profile the device is in");
    CHECK_EQ(runtime.state().state(), ApplyState::Unsupported);
}

TEST("runtime: an unvalidated pack is inert and reports capability-limited, writing nothing") {
    // Every vendor family ships PhysicalDeviceRequired. Until someone validates it on real
    // hardware of that family, it must not write.
    TempTree tree;
    ExecutionRuntime runtime({generic_on(tree, /*validated=*/false)}, {SocFamily::Generic, "fake"},
                             tree.policy());

    const auto cycle = runtime.on_decision(performance_decision(), game_session(), 1000);

    CHECK_MSG(tree.governor0() == "schedutil",
              "an unvalidated capability must not write, got: " + tree.governor0());
    CHECK(!cycle.apply.verified_active);
    CHECK_MSG(runtime.state().state() != ApplyState::Verified,
              "an inert runtime must never report a verified profile");
    CHECK_MSG(!runtime.state().fully_optimized(), "nothing was applied; nothing is optimized");
}

TEST("runtime: a real vendor pack stays gated on an unvalidated device") {
    // Not a fabricated descriptor: the actual shipped MediaTek pack. It must be inert.
    TempTree tree;
    // packs_for() already includes the generic pack; the vendor pack rides on top of it.
    auto packs = flux::device::packs_for(SocFamily::MediaTek);
    for (auto &pack : packs) {
        const bool is_vendor = pack.provenance == Provenance::DerivedEncore;
        for (auto &d : pack.descriptors) {
            if (is_vendor) {
                // Seed the vendor node so the descriptor is gated by its *validation status* and
                // nothing else. A missing node would prove nothing: it would be inert for the
                // wrong reason. (MediaTek's nodes live under /proc/ppm and /proc/gpufreq.)
                tree.write_node(d.path, "0");
            } else {
                d.validation = ValidationStatus::PhysicalDeviceValidated; // generic is validated
            }
            d.path = tree.rebase(d.path);
        }
    }

    ExecutionRuntime runtime(packs, {SocFamily::MediaTek, "fake"}, tree.policy());
    const auto cycle = runtime.on_decision(performance_decision(), game_session(), 1000);

    // The generic fallback still works underneath the gated vendor pack.
    CHECK_MSG(tree.governor0() == "performance",
              "the generic fallback must survive a gated vendor pack, got: " + tree.governor0());
    CHECK_MSG(!runtime.state().fully_optimized(),
              "vendor tuning was not applied, so Flux must not claim full optimization");
    CHECK_MSG(cycle.plan.prevented.size() > 0,
              "every prevented vendor action must be recorded, not silently dropped");
}

TEST("runtime: a failed apply leaves the previously verified profile alone") {
    TempTree tree;
    ExecutionRuntime runtime({generic_on(tree)}, {SocFamily::Generic, "fake"}, tree.policy());

    // Settle on Performance first.
    (void)runtime.on_decision(performance_decision(), game_session(), 1000);
    CHECK_EQ(runtime.state().verified_profile(), eng::TargetProfile::Performance);

    // Now make the nodes unwritable and ask for something else.
    for (int policy : {0, 4, 7}) {
        const std::string node = "/sys/devices/system/cpu/cpufreq/policy" +
                                 std::to_string(policy) + "/scaling_governor";
        std::filesystem::permissions(tree.rebase(node), std::filesystem::perms::none);
    }
    runtime.invalidate_capabilities("test");
    const auto cycle = runtime.on_decision(
        decide(eng::TargetProfile::PowerSave, eng::DecisionReason::battery_saver_enabled,
               eng::DecisionPriority::BatterySaver),
        SessionContext{}, 2000);

    CHECK_MSG(!cycle.apply.verified_active, "an unwritable device cannot verify anything");
    CHECK_MSG(runtime.state().verified_profile() == eng::TargetProfile::Performance,
              "a failed apply must not rewrite history: the device is still in what was last "
              "actually verified");
}

// --- idempotency and coalescing --------------------------------------------

TEST("runtime: an identical repeated decision coalesces and writes nothing") {
    TempTree tree;
    ExecutionRuntime runtime({generic_on(tree)}, {SocFamily::Generic, "fake"}, tree.policy());

    (void)runtime.on_decision(performance_decision(), game_session(), 1000);
    const auto again = runtime.on_decision(performance_decision(), game_session(), 1100);

    CHECK_MSG(again.coalesced, "an unchanged ask against an unchanged device is not work");
    CHECK_MSG(!again.applied, "a coalesced cycle must not reach the engine at all");
}

TEST("runtime: a safety downgrade is never coalesced") {
    // The one thing coalescing may not do. A thermal emergency must reach the device even if
    // the runtime believes nothing has changed.
    TempTree tree;
    ExecutionRuntime runtime({generic_on(tree)}, {SocFamily::Generic, "fake"}, tree.policy());

    auto emergency = decide(eng::TargetProfile::Balanced, eng::DecisionReason::thermal_emergency,
                            eng::DecisionPriority::ThermalEmergency);
    emergency.safety_driven = true;

    (void)runtime.on_decision(emergency, game_session(), 1000);
    const auto again = runtime.on_decision(emergency, game_session(), 1100);

    CHECK_MSG(!again.coalesced, "a safety response must never be held back by a state comparison");
    CHECK(again.applied);
}

TEST("runtime: a capability generation change forces a re-apply") {
    TempTree tree;
    ExecutionRuntime runtime({generic_on(tree)}, {SocFamily::Generic, "fake"}, tree.policy());

    (void)runtime.on_decision(performance_decision(), game_session(), 1000);
    const uint64_t before = runtime.capability_generation();

    runtime.invalidate_capabilities("configuration changed");
    const auto again = runtime.on_decision(performance_decision(), game_session(), 1100);

    CHECK_MSG(runtime.capability_generation() > before, "invalidation must move the generation");
    CHECK_MSG(!again.coalesced, "an invalidated runtime must not coalesce");
    CHECK(again.apply.verified_active);
}

TEST("runtime: external mutation invalidates the verified cache and re-applies") {
    TempTree tree;
    ExecutionRuntime runtime({generic_on(tree)}, {SocFamily::Generic, "fake"}, tree.policy());
    (void)runtime.on_decision(performance_decision(), game_session(), 1000);

    // Something outside Flux moves the node.
    tree.write_node("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", "powersave");

    const auto drifted = runtime.poll_external_mutation(1100);

    CHECK_MSG(!drifted.empty(), "a node Flux verified and no longer holds must be detected");
    CHECK_EQ(runtime.state().state(), ApplyState::ExternalMutation);
    CHECK_MSG(!runtime.state().has_verified_profile(),
              "Flux cannot claim a verified profile once the device has drifted from it");

    const auto again = runtime.on_decision(performance_decision(), game_session(), 1200);
    CHECK_MSG(!again.coalesced, "drift must force real work, not a coalesced no-op");
    CHECK_EQ(tree.governor0(), std::string("performance"));
}

// --- zen -------------------------------------------------------------------

TEST("runtime: zen engages the exact mode and restores the exact original") {
    for (const int mode : {0, 1, 2, 3}) {
        TempTree tree;
        FakeZen zen(/*initial=*/3); // the user is in alarms-only
        ExecutionRuntime runtime({generic_on(tree)}, {SocFamily::Generic, "fake"}, tree.policy(),
                                 &zen);

        (void)runtime.on_decision(performance_decision(), game_session(true, mode), 1000);
        CHECK_MSG(zen.read().value() == mode,
                  "zen must engage the exact requested mode " + std::to_string(mode));

        // Session ends: zen must go back to exactly 3, not to "off" and not to "priority".
        (void)runtime.on_decision(decide(eng::TargetProfile::Balanced,
                                         eng::DecisionReason::session_ended,
                                         eng::DecisionPriority::SessionLifecycle),
                                  SessionContext{}, 2000);
        CHECK_MSG(zen.read().value() == 3,
                  "the exact original mode must come back, got " + std::to_string(zen.read().value()));
    }
}

TEST("runtime: an unavailable zen capability is never written") {
    TempTree tree;
    FakeZen zen(std::nullopt); // telemetry cannot tell us the mode
    ExecutionRuntime runtime({generic_on(tree)}, {SocFamily::Generic, "fake"}, tree.policy(), &zen);

    (void)runtime.on_decision(performance_decision(), game_session(true, 1), 1000);

    CHECK_MSG(zen.writes == 0, "no zen capability means no zen write, not a guessed one");
}

TEST("runtime: a user's own zen change is not overwritten by Flux's restore") {
    TempTree tree;
    FakeZen zen(0);
    ExecutionRuntime runtime({generic_on(tree)}, {SocFamily::Generic, "fake"}, tree.policy(), &zen);

    (void)runtime.on_decision(performance_decision(), game_session(true, 1), 1000);
    CHECK_EQ(zen.read().value(), 1);

    // The user deliberately switches to total silence while the game runs.
    zen.user_changes_to(2);

    (void)runtime.on_decision(decide(eng::TargetProfile::Balanced,
                                     eng::DecisionReason::session_ended,
                                     eng::DecisionPriority::SessionLifecycle),
                              SessionContext{}, 2000);

    CHECK_MSG(zen.read().value() == 2,
              "the user changed zen themselves; their choice outranks Flux putting it back");
}

TEST("runtime: no zen backend at all means zen is simply never touched") {
    TempTree tree;
    ExecutionRuntime runtime({generic_on(tree)}, {SocFamily::Generic, "fake"}, tree.policy(),
                             /*zen=*/nullptr);

    const auto cycle = runtime.on_decision(performance_decision(), game_session(true, 1), 1000);

    CHECK_MSG(cycle.apply.verified_active, "a device without zen still applies its profile");
    CHECK(runtime.zen() == nullptr);
}

// --- restoration -----------------------------------------------------------

TEST("runtime: restore_all returns every value Flux changed, exactly") {
    TempTree tree;
    FakeZen zen(3);
    ExecutionRuntime runtime({generic_on(tree)}, {SocFamily::Generic, "fake"}, tree.policy(), &zen);

    (void)runtime.on_decision(performance_decision(), game_session(true, 1), 1000);
    CHECK_EQ(tree.governor0(), std::string("performance"));

    const auto restored = runtime.restore_all("daemon_stopping", 2000);

    CHECK_MSG(restored.verified_active, "restore must report what it achieved: " + restored.message);
    CHECK_MSG(tree.governor0() == "schedutil", "the device must come back to its own value");
    CHECK_MSG(zen.read().value() == 3, "zen must come back to the user's exact original");
    CHECK_MSG(!runtime.state().has_verified_profile(),
              "after a restore the device is in its own state, not a Flux profile");
}
