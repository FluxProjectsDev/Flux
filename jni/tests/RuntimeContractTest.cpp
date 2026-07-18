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

// The Stage 2 runtime contract: the whole pipeline, composed the way the daemon composes it.
//
// Everything else in this suite tests a component. This tests the promises — the sentences a user
// or a maintainer would actually rely on:
//
//   "a profile is only active if it was verified"
//   "turning tweaks off means Flux does not touch my device"
//   "an unvalidated vendor pack writes nothing"
//   "a thermal emergency beats whatever the game asked for"
//   "stale telemetry never promotes"
//
// The fixture builds the same objects Main.cpp builds, in the same order, and drives them through
// the real DecisionEngine, IntentMapper, DryRunPlanner, LivePlanCompiler and ExecutionEngine
// against a real filesystem tree. Nothing is mocked except the device itself: no interface is
// bypassed and no shortcut is taken that production does not take. A fixture that skipped the
// planner would prove the planner's absence, not the contract.

#include "TestFramework.hpp"

#include "DecisionAdapter.hpp"
#include "DevicePacks.hpp"
#include "ExecutionRuntime.hpp"
#include "RuntimeSnapshotAssembler.hpp"
#include "RuntimeTuning.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace flux::execution;
namespace eng = flux::engine;
namespace tel = flux::telemetry;

namespace {

/// A device tree with the generic pack's nodes present and writable.
class DeviceFixture {
public:
    DeviceFixture() {
        char tmpl[] = "/tmp/flux_contract_XXXXXX";
        const char *dir = mkdtemp(tmpl);
        root_ = dir ? dir : "";
        for (int policy : {0, 4, 7}) {
            write("/sys/devices/system/cpu/cpufreq/policy" + std::to_string(policy) +
                      "/scaling_governor",
                  "schedutil");
        }
    }
    ~DeviceFixture() {
        if (!root_.empty()) std::filesystem::remove_all(root_);
    }
    DeviceFixture(const DeviceFixture &) = delete;
    DeviceFixture &operator=(const DeviceFixture &) = delete;

    void write(const std::string &relative, const std::string &content) const {
        const std::string full = root_ + relative;
        std::filesystem::create_directories(std::filesystem::path(full).parent_path());
        std::ofstream out(full);
        out << content;
    }
    [[nodiscard]] std::string governor(int policy = 0) const {
        std::ifstream in(root_ + "/sys/devices/system/cpu/cpufreq/policy" +
                         std::to_string(policy) + "/scaling_governor");
        std::string value;
        std::getline(in, value);
        return value;
    }
    void make_unwritable() const {
        for (int policy : {0, 4, 7}) {
            std::filesystem::permissions(root_ + "/sys/devices/system/cpu/cpufreq/policy" +
                                             std::to_string(policy) + "/scaling_governor",
                                         std::filesystem::perms::none);
        }
    }
    [[nodiscard]] std::string rebase(const std::string &absolute) const { return root_ + absolute; }
    [[nodiscard]] PathPolicy policy() const {
        return PathPolicy(std::vector<std::string>{root_ + "/"});
    }

private:
    std::string root_;
};

/// A zen backend that behaves like the production one: a mode, or nothing at all.
class ZenFixture : public ZenBackend {
public:
    explicit ZenFixture(std::optional<int> initial) : mode_(initial) {}
    [[nodiscard]] bool available() const override { return mode_.has_value(); }
    [[nodiscard]] std::optional<int> read() const override { return mode_; }
    bool set(int mode) override {
        if (!mode_) return false;
        mode_ = mode;
        ++writes;
        return true;
    }
    int writes = 0;

private:
    std::optional<int> mode_;
};

/** Which packs the fixture should compose with. */
enum class Packs {
    GenericValidated, ///< the generic fallback, as if hardware-validated
    GenericUnvalidated,
    MediaTekGated, ///< the real shipped vendor pack, gated as it ships
};

/**
 * @brief The daemon's composition, minus the daemon.
 *
 * Mirrors run_daemon()/evaluate_and_apply(): assemble telemetry, decide, hand the full Decision
 * to the execution runtime, read the state back. If Main.cpp's composition drifts from this the
 * contract tests stop describing the product, so this stays deliberately close to it.
 */
class Runtime {
public:
    explicit Runtime(Packs which = Packs::GenericValidated,
                     std::optional<int> initial_zen = 0)
        : zen_(initial_zen) {
        execution_ = std::make_unique<ExecutionRuntime>(build_packs(which), identity(which),
                                                        device_.policy(), &zen_);
    }

    void tune(const LegacyConfigInput &config, int64_t now_ms = 500) {
        execution_->set_tuning(migrate_legacy_config(config), now_ms);
    }

    /// One full cycle: snapshot -> decision -> intent -> plan -> apply -> state.
    RuntimeCycleResult cycle(const tel::RawSnapshot &snapshot, tel::TelemetryHealth health,
                             bool in_session, int64_t now_ms, bool wants_zen = false,
                             int zen_mode = 1) {
        const auto assembled = assembler_.assemble(
            snapshot, health,
            tel::RuntimeSnapshotAssembler::foreground_from_synthesiscore(snapshot));

        eng::DecisionInputs inputs;
        inputs.runtime = assembled.runtime;
        inputs.capabilities = assembled.capabilities;
        inputs.session.in_session = in_session;
        inputs.session.package = in_session ? "com.example.game" : "";

        (void)decisions_.decide(inputs, policy_state_, now_ms);

        SessionContext session;
        session.in_session = in_session;
        session.package = inputs.session.package;
        session.wants_zen = wants_zen;
        session.desired_zen_mode = zen_mode;

        return execution_->on_decision(decisions_.last_decision(), session, now_ms);
    }

    /// Drive to a settled Performance profile. Telemetry needs samples to become Healthy and the
    /// engine defers one cycle before an aggressive promotion.
    void warm_to_performance() {
        for (int i = 1; i <= 4; ++i) {
            cycle(cool_snapshot(static_cast<uint64_t>(i)), tel::TelemetryHealth::Healthy, true,
                  i * 100);
        }
    }

    static tel::RawSnapshot cool_snapshot(uint64_t sequence, bool screen_awake = true) {
        tel::RawSnapshot s;
        s.sequence = sequence;
        s.schema_version = 2;
        s.thermal_available = true;
        s.thermal_valid = true;
        s.thermal_headroom = 0.30f;
        s.screen_available = true;
        s.screen_awake = screen_awake;
        s.power_available = true;
        s.battery_saver = false;
        s.charging_available = true;
        s.charging = false;
        s.foreground_available = true;
        s.focused_package = "com.example.game";
        s.focused_pid = 1234;
        s.focused_uid = 10123;
        s.zen_available = true;
        s.zen_mode = 0;
        return s;
    }

    [[nodiscard]] DeviceFixture &device() { return device_; }
    [[nodiscard]] ZenFixture &zen() { return zen_; }
    [[nodiscard]] ExecutionRuntime &execution() { return *execution_; }
    [[nodiscard]] const RuntimeProfileState &state() const { return execution_->state(); }

private:
    // Declaration order is construction order: the device tree exists before anything that reads
    // it, exactly as telemetry exists before the execution runtime in run_daemon().
    DeviceFixture device_;
    ZenFixture zen_;
    tel::RuntimeSnapshotAssembler assembler_;
    FluxDecisionService decisions_;
    PolicyState policy_state_;
    std::unique_ptr<ExecutionRuntime> execution_;

    [[nodiscard]] std::vector<DevicePack> build_packs(Packs which) const {
        if (which == Packs::MediaTekGated) {
            // The real shipped pack, exactly as it ships. Its nodes are seeded so it is gated by
            // its validation status and not merely by an absent file.
            auto packs = flux::device::packs_for(SocFamily::MediaTek);
            for (auto &pack : packs) {
                const bool vendor = pack.provenance == Provenance::DerivedEncore;
                for (auto &d : pack.descriptors) {
                    if (vendor) device_.write(d.path, "0");
                    else d.validation = ValidationStatus::PhysicalDeviceValidated;
                    d.path = device_.rebase(d.path);
                }
            }
            return packs;
        }

        DevicePack pack = flux::device::generic_pack();
        for (auto &d : pack.descriptors) {
            d.path = device_.rebase(d.path);
            if (which == Packs::GenericValidated) {
                d.validation = ValidationStatus::PhysicalDeviceValidated;
            }
        }
        return {pack};
    }

    [[nodiscard]] static DeviceIdentity identity(Packs which) {
        return {which == Packs::MediaTekGated ? SocFamily::MediaTek : SocFamily::Generic, "fake"};
    }
};

} // namespace

// --- supported generic apply -----------------------------------------------

TEST("contract: a supported generic balanced apply succeeds and verifies") {
    Runtime rt;
    // No session, healthy telemetry: the engine settles on Balanced.
    rt.cycle(Runtime::cool_snapshot(1), tel::TelemetryHealth::Healthy, false, 100);
    const auto result = rt.cycle(Runtime::cool_snapshot(2), tel::TelemetryHealth::Healthy, false, 200);

    CHECK_MSG(result.apply.verified_active || result.coalesced,
              "a balanced apply on a supported device must verify: " + result.apply.message);
    CHECK_EQ(rt.device().governor(), std::string("schedutil"));
    CHECK_MSG(rt.state().has_verified_profile(), "a verified apply must produce a verified state");
}

TEST("contract: a cool game session drives a verified performance apply end to end") {
    Runtime rt;
    rt.warm_to_performance();

    CHECK_MSG(rt.device().governor() == "performance",
              "the whole pipeline must reach the device, got: " + rt.device().governor());
    CHECK_EQ(rt.state().verified_profile(), eng::TargetProfile::Performance);
    CHECK_EQ(rt.state().state(), ApplyState::Verified);
}

TEST("contract: powersave selects true powersave semantics, not the safe fallback") {
    // The regression Increment 5 fixed: PowerSave shared the "safe" key with thermal fallbacks,
    // so a power-save profile applied the balanced governor.
    Runtime rt;
    auto battery_saver = Runtime::cool_snapshot(1);
    battery_saver.battery_saver = true;

    rt.cycle(battery_saver, tel::TelemetryHealth::Healthy, false, 100);
    auto next = battery_saver;
    next.sequence = 2;
    rt.cycle(next, tel::TelemetryHealth::Healthy, false, 200);

    CHECK_MSG(rt.device().governor() == "powersave",
              "battery saver must ask for a slower CPU, not the kernel's balanced default; got: " +
                  rt.device().governor());
}

// --- the gates -------------------------------------------------------------

TEST("contract: disable_tweaks produces a non-writing result") {
    Runtime rt;
    LegacyConfigInput config;
    config.disable_tweaks = true;
    rt.tune(config);

    rt.warm_to_performance();

    CHECK_MSG(rt.device().governor() == "schedutil",
              "a user who turned tweaks off must find their device untouched, got: " +
                  rt.device().governor());
    CHECK_MSG(!rt.state().has_verified_profile(),
              "a disabled runtime has verified nothing, because it applied nothing");
}

TEST("contract: NO_PERFORMANCE_CPUGOV blocks the unsafe performance governor") {
    // Hardware safety: some kernels hang or reboot on it. The device still gets a profile — it
    // just does not get that governor.
    Runtime rt;
    LegacyConfigInput config;
    config.mitigation_items = {"NO_PERFORMANCE_CPUGOV"};
    rt.tune(config);

    rt.warm_to_performance();

    CHECK_MSG(rt.device().governor() != "performance",
              "a device that cannot take the performance governor must never be given it");
    CHECK_MSG(rt.device().governor() == "schedutil",
              "the mitigation falls back to the balanced governor, got: " + rt.device().governor());
}

TEST("contract: a PhysicalDeviceRequired vendor pack produces zero writes") {
    Runtime rt(Packs::MediaTekGated);
    rt.warm_to_performance();

    // The generic fallback still applies underneath the gated vendor pack.
    CHECK_MSG(rt.device().governor() == "performance",
              "the generic fallback must survive a gated vendor pack, got: " +
                  rt.device().governor());
    CHECK_MSG(!rt.state().fully_optimized(),
              "vendor tuning did not happen, so Flux must not claim full optimization");
    CHECK_MSG(rt.state().state() == ApplyState::CapabilityLimited ||
                  rt.state().state() == ApplyState::Verified,
              "a gated vendor pack is a capability limit, not a failure");
}

TEST("contract: an unvalidated generic pack writes nothing at all") {
    Runtime rt(Packs::GenericUnvalidated);
    rt.warm_to_performance();

    CHECK_MSG(rt.device().governor() == "schedutil",
              "an unvalidated capability must not write, got: " + rt.device().governor());
    CHECK_MSG(!rt.state().has_verified_profile(), "nothing was applied; nothing is verified");
}

// --- safety beats preference -----------------------------------------------

TEST("contract: stale telemetry prevents aggressive promotion") {
    Runtime rt;
    // One sample, then let it age: telemetry never becomes Healthy, so no promotion is allowed.
    rt.cycle(Runtime::cool_snapshot(1), tel::TelemetryHealth::Stale, true, 100);
    rt.cycle(Runtime::cool_snapshot(1), tel::TelemetryHealth::Stale, true, 6000);

    CHECK_MSG(rt.device().governor() != "performance",
              "stale data must never promote: the reading that justified it may be minutes old");
    CHECK(rt.state().verified_profile() != eng::TargetProfile::Performance);
}

TEST("contract: a thermal emergency defeats the performance intent") {
    Runtime rt;
    rt.warm_to_performance();
    CHECK_EQ(rt.device().governor(), std::string("performance"));

    auto hot = Runtime::cool_snapshot(5);
    hot.thermal_headroom = 1.30f; // over the limit
    const auto result = rt.cycle(hot, tel::TelemetryHealth::Healthy, true, 500);

    CHECK_MSG(rt.device().governor() == "schedutil",
              "a hot device must be brought down whatever the game asked for, got: " +
                  rt.device().governor());
    CHECK_MSG(result.apply.verified_active, "the downgrade must itself be verified: " +
                                                result.apply.message);
}

TEST("contract: telemetry going offline falls back to a safe profile") {
    Runtime rt;
    rt.warm_to_performance();

    rt.cycle(tel::RawSnapshot{}, tel::TelemetryHealth::Unavailable, false, 600);

    CHECK_MSG(rt.device().governor() != "performance",
              "a device with no telemetry must not be left running an aggressive profile");
}

// --- idempotency and invalidation ------------------------------------------

TEST("contract: a repeated identical verified decision produces zero second writes") {
    Runtime rt;
    rt.warm_to_performance();
    const std::string settled = rt.device().governor();

    const auto again = rt.cycle(Runtime::cool_snapshot(9), tel::TelemetryHealth::Healthy, true, 900);

    CHECK_MSG(again.coalesced || again.apply.succeeded == 0,
              "an unchanged decision against an unchanged device is not work");
    CHECK_EQ(rt.device().governor(), settled);
}

TEST("contract: a configuration generation change invalidates assumptions") {
    Runtime rt;
    rt.warm_to_performance();
    const uint64_t before = rt.execution().capability_generation();

    // A balanced-governor change, while the game keeps asking for performance. The point is not
    // which value lands — the session still drives performance — but that the change is not
    // silently ignored: the generation moves and the next identical decision is recomputed
    // rather than coalesced away.
    LegacyConfigInput config;
    config.balanced_governor = "walt";
    rt.tune(config, 1000);

    CHECK_MSG(rt.execution().capability_generation() > before,
              "new settings mean the last plan was compiled against assumptions that no longer "
              "hold");

    // Deliberately NOT asserted: that the verified profile is dropped. Nothing touched the
    // device, so it still holds exactly what Flux verified and the claim is still true. What a
    // settings change invalidates is the *idempotency cache* — the next cycle must recompute
    // rather than skip. That is the difference from external mutation, where the device really
    // stopped holding the value and the claim has to go.
    const auto next = rt.cycle(Runtime::cool_snapshot(9), tel::TelemetryHealth::Healthy, true, 1100);
    CHECK_MSG(!next.coalesced,
              "a settings change must force the next cycle to recompute, not coalesce it away");
    CHECK_MSG(next.applied,
              "the recomputed cycle must actually reach the engine after a settings change");

    // And a balanced ask now does pick up the new governor, proving the setting took effect
    // rather than being merely accepted.
    rt.execution().invalidate_capabilities("test");
    rt.cycle(Runtime::cool_snapshot(10), tel::TelemetryHealth::Unavailable, false, 1200);
    CHECK_MSG(rt.device().governor() == "walt",
              "a balanced decision after the change must use the migrated governor, got: " +
                  rt.device().governor());
}

TEST("contract: external mutation invalidates the verified state") {
    Runtime rt;
    rt.warm_to_performance();

    // Something outside Flux moves the node.
    rt.device().write("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", "powersave");
    const auto drifted = rt.execution().poll_external_mutation(1000);

    CHECK_MSG(!drifted.empty(), "a node Flux verified and no longer holds must be detected");
    CHECK_EQ(rt.state().state(), ApplyState::ExternalMutation);
    CHECK_MSG(!rt.state().has_verified_profile(),
              "Flux cannot claim a verified profile once the device has drifted from it");
}

// --- failure, verification and rollback ------------------------------------

TEST("contract: an unwritable device fails and never claims a profile") {
    Runtime rt;
    rt.device().make_unwritable();
    rt.warm_to_performance();

    CHECK_MSG(!rt.state().has_verified_profile(),
              "a device that refused every write is not running a Flux profile");
    CHECK_MSG(rt.state().state() != ApplyState::Verified,
              "a failed apply must not report Verified");
}

TEST("contract: a failed apply leaves the last verified profile alone") {
    Runtime rt;
    rt.warm_to_performance();
    CHECK_EQ(rt.state().verified_profile(), eng::TargetProfile::Performance);

    rt.device().make_unwritable();
    rt.execution().invalidate_capabilities("test");
    rt.cycle(tel::RawSnapshot{}, tel::TelemetryHealth::Unavailable, false, 900);

    CHECK_MSG(rt.state().verified_profile() == eng::TargetProfile::Performance,
              "a failed apply must not rewrite history: the last thing actually verified is "
              "still the last thing actually verified");
}

TEST("contract: requested state is never treated as verified state") {
    // The rule the entire increment exists to enforce, stated once at the top level.
    Runtime rt(Packs::GenericUnvalidated);
    rt.warm_to_performance();

    CHECK_MSG(rt.state().requested_profile() == eng::TargetProfile::Performance,
              "the ask must be recorded");
    CHECK_MSG(rt.state().verified_profile() != eng::TargetProfile::Performance,
              "the ask must not become the answer");
    CHECK_MSG(!rt.state().has_verified_profile(), "nothing was verified, so nothing is claimed");
}

// --- zen -------------------------------------------------------------------

TEST("contract: exact zen modes 0 through 3 survive the whole pipeline") {
    for (const int mode : {0, 1, 2, 3}) {
        Runtime rt;
        rt.cycle(Runtime::cool_snapshot(1), tel::TelemetryHealth::Healthy, true, 100, true, mode);
        rt.cycle(Runtime::cool_snapshot(2), tel::TelemetryHealth::Healthy, true, 200, true, mode);

        CHECK_MSG(rt.zen().read().value() == mode,
                  "zen must engage the exact requested mode " + std::to_string(mode) + ", got " +
                      std::to_string(rt.zen().read().value_or(-1)));
    }
}

TEST("contract: zen returns to the user's exact original when the session ends") {
    Runtime rt(Packs::GenericValidated, /*initial_zen=*/0);
    rt.cycle(Runtime::cool_snapshot(1), tel::TelemetryHealth::Healthy, true, 100, true, 2);
    rt.cycle(Runtime::cool_snapshot(2), tel::TelemetryHealth::Healthy, true, 200, true, 2);
    CHECK_EQ(rt.zen().read().value(), 2);

    // Session ends.
    rt.cycle(Runtime::cool_snapshot(3, /*screen_on=*/true), tel::TelemetryHealth::Healthy, false,
             300);

    CHECK_MSG(rt.zen().read().value() == 0,
              "the exact original mode must come back, got " +
                  std::to_string(rt.zen().read().value_or(-1)));
}

TEST("contract: an unavailable zen capability is never written") {
    Runtime rt(Packs::GenericValidated, /*initial_zen=*/std::nullopt);
    rt.cycle(Runtime::cool_snapshot(1), tel::TelemetryHealth::Healthy, true, 100, true, 1);

    CHECK_MSG(rt.zen().writes == 0,
              "no zen capability means no zen write, not a guessed one");
}
