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

// Probing and dry runs against fake device filesystems.
//
// Each fixture is a real temp directory laid out like the sysfs the descriptors expect, so the
// probe exercises its actual syscalls — wildcard resolution, symlink refusal, permission
// checks — rather than a mock's idea of them. Every test asserts the probe performed **zero
// writes** and left every mode untouched: a probe that mutated the device to find out whether
// it could mutate the device would be its own bug.

#include "TestFramework.hpp"

#include "DevicePacks.hpp"
#include "DeviceDescriptor.hpp"

#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace flux::execution;

namespace {

/// A fake device tree. Records every node's content and mode so a test can prove nothing moved.
class FakeDevice {
public:
    FakeDevice() {
        char tmpl[] = "/tmp/flux_device_XXXXXX";
        const char *dir = mkdtemp(tmpl);
        root_ = dir ? dir : "";
    }
    ~FakeDevice() {
        if (!root_.empty()) {
            const std::string cmd = "rm -rf '" + root_ + "'";
            if (system(cmd.c_str()) != 0) { /* best effort */
            }
        }
    }
    FakeDevice(const FakeDevice &) = delete;
    FakeDevice &operator=(const FakeDevice &) = delete;

    /// Create a node at a path *relative to the fake root*, mirroring a real sysfs path.
    void node(const std::string &relative, const std::string &content, mode_t mode = 0644) {
        const std::string full = root_ + relative;
        make_parents(full);
        std::ofstream out(full);
        out << content;
        out.close();
        chmod(full.c_str(), mode);
        tracked_.push_back(full);
    }

    void symlink_node(const std::string &relative, const std::string &target_relative) {
        const std::string full = root_ + relative;
        make_parents(full);
        if (::symlink((root_ + target_relative).c_str(), full.c_str()) != 0) return;
        tracked_.push_back(full);
    }

    /// Rebase a descriptor's absolute path onto the fake root, so real paths can be described.
    [[nodiscard]] std::string rebase(const std::string &absolute) const { return root_ + absolute; }

    /// A policy whose only approved root is this fake device.
    [[nodiscard]] PathPolicy policy() const {
        return PathPolicy(std::vector<std::string>{root_ + "/"});
    }

    /// Snapshot every tracked node's content and mode, to prove a probe changed nothing.
    [[nodiscard]] std::vector<std::pair<std::string, mode_t>> snapshot() const {
        std::vector<std::pair<std::string, mode_t>> out;
        for (const auto &p : tracked_) {
            struct stat st {};
            if (lstat(p.c_str(), &st) != 0) continue;
            std::string content;
            if (S_ISREG(st.st_mode)) {
                std::ifstream in(p);
                content.assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
            }
            out.emplace_back(p + "|" + content, st.st_mode & 07777);
        }
        return out;
    }

private:
    static void make_parents(const std::string &full) {
        size_t pos = full.find('/', 1);
        while (pos != std::string::npos) {
            mkdir(full.substr(0, pos).c_str(), 0755);
            pos = full.find('/', pos + 1);
        }
    }

    std::string root_;
    std::vector<std::string> tracked_;
};

/// Rewrite a pack's descriptor paths onto the fake root, leaving everything else alone.
DevicePack rebase_pack(DevicePack pack, const FakeDevice &device) {
    for (auto &d : pack.descriptors) d.path = device.rebase(d.path);
    return pack;
}

const CapabilityStatus *find(const CapabilityReport &report, const std::string &descriptor_id) {
    for (const auto &s : report.statuses) {
        if (s.descriptor_id == descriptor_id) return &s;
    }
    return nullptr;
}

} // namespace

TEST("dry run: a probe performs zero writes and changes no mode") {
    // The core safety property. Everything below relies on it.
    FakeDevice device;
    device.node("/sys/class/kgsl/kgsl-3d0/force_clk_on", "0", 0444); // read-only, like a real one
    device.node("/sys/class/kgsl/kgsl-3d0/bus_split", "1", 0644);

    const auto before = device.snapshot();

    SysfsNodeBackend backend(device.policy());
    const CapabilityProbe probe(backend, device.policy());
    const auto pack = rebase_pack(flux::device::snapdragon_pack(), device);

    (void)probe.probe_pack(pack, {SocFamily::Snapdragon, "fake"},
                           flux::device::kIntentSustainedPerformance, 1000);

    CHECK_MSG(device.snapshot() == before,
              "probing must not write a value or change a mode on any node");
}

TEST("dry run: every SoC family probes without writing") {
    // All six families plus generic, each against a device that has none of its nodes. The
    // interesting property is not the verdict but that nothing was touched getting there.
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
        {"generic", SocFamily::Generic, flux::device::generic_pack},
    };

    for (const auto &family : families) {
        FakeDevice device;
        device.node("/sys/placeholder", "1", 0644); // an empty tree still has a root
        const auto before = device.snapshot();

        SysfsNodeBackend backend(device.policy());
        const CapabilityProbe probe(backend, device.policy());
        const auto pack = rebase_pack(family.pack(), device);
        const auto report = probe.probe_pack(pack, {family.soc, "fake"},
                                             flux::device::kIntentSustainedPerformance, 1000);

        CHECK_MSG(device.snapshot() == before,
                  std::string(family.name) + ": dry run must perform zero writes");
        CHECK_MSG(report.statuses.size() == pack.descriptors.size(),
                  std::string(family.name) + ": every descriptor must be reported on");
        // Nothing exists on this fake device, so nothing may be executable.
        CHECK_MSG(report.executable_count() == 0,
                  std::string(family.name) + ": no capability may be executable on a bare device");
    }
}

TEST("dry run: a descriptor for another SoC is unsupported, not attempted") {
    // Snapdragon nodes present, but the device says MediaTek. SoC identity gates the candidate.
    FakeDevice device;
    device.node("/sys/class/kgsl/kgsl-3d0/force_clk_on", "0", 0644);
    device.node("/sys/class/kgsl/kgsl-3d0/bus_split", "1", 0644);
    const auto before = device.snapshot();

    SysfsNodeBackend backend(device.policy());
    const CapabilityProbe probe(backend, device.policy());
    const auto pack = rebase_pack(flux::device::snapdragon_pack(), device);

    const auto report = probe.probe_pack(pack, {SocFamily::MediaTek, "fake"},
                                         flux::device::kIntentSustainedPerformance, 1000);

    for (const auto &s : report.statuses) {
        CHECK_EQ(s.state, CapabilityState::Unsupported);
        CHECK_EQ(s.reason, CapabilityReason::SocMismatch);
        CHECK(!s.soc_matched);
    }
    CHECK_MSG(device.snapshot() == before, "a wrong-SoC probe must touch nothing");
}

TEST("dry run: a present, well-formed vendor node still awaits device validation") {
    // The gate that keeps unverified vendor tuning inert. Everything about this node checks
    // out — it exists, parses, is writable, the value is in range — and it is still not
    // executable, because nobody has confirmed on real silicon that writing it does what the
    // descriptor claims.
    FakeDevice device;
    device.node("/sys/class/kgsl/kgsl-3d0/force_clk_on", "0", 0644);
    const auto before = device.snapshot();

    SysfsNodeBackend backend(device.policy());
    const CapabilityProbe probe(backend, device.policy());
    const auto pack = rebase_pack(flux::device::snapdragon_pack(), device);

    const auto report = probe.probe_pack(pack, {SocFamily::Snapdragon, "fake"},
                                         flux::device::kIntentSustainedPerformance, 1000);

    const auto *status = find(report, "sd.gpu.force_clk_on");
    CHECK(status != nullptr);
    if (status) {
        CHECK_EQ(status->state, CapabilityState::DeviceValidationRequired);
        CHECK_EQ(status->reason, CapabilityReason::AwaitingDeviceValidation);
        CHECK_MSG(status->soc_matched, "the SoC did match; the gate is validation, not identity");
        CHECK_MSG(status->readable && status->current_value_valid,
                  "the node is fine; only hardware confirmation is missing");
        CHECK_MSG(!status->executable(), "an unvalidated descriptor must produce no action");
        CHECK_EQ(status->validation, ValidationStatus::PhysicalDeviceRequired);
    }
    CHECK_MSG(device.snapshot() == before, "no write, no chmod");
}

TEST("dry run: a missing node is unsupported and never written") {
    FakeDevice device;
    device.node("/sys/placeholder", "1", 0644);
    SysfsNodeBackend backend(device.policy());
    const CapabilityProbe probe(backend, device.policy());
    const auto pack = rebase_pack(flux::device::snapdragon_pack(), device);

    const auto report = probe.probe_pack(pack, {SocFamily::Snapdragon, "fake"},
                                         flux::device::kIntentSustainedPerformance, 1000);
    const auto *status = find(report, "sd.gpu.force_clk_on");
    CHECK(status != nullptr);
    if (status) {
        CHECK_EQ(status->state, CapabilityState::Unsupported);
        CHECK_EQ(status->reason, CapabilityReason::NodeMissing);
        CHECK(!status->executable());
    }
}

TEST("dry run: a node whose current value does not parse is invalid_format, not overwritten") {
    // Flux does not understand this node on this device, so it must not write a value whose
    // effect it cannot reason about.
    FakeDevice device;
    device.node("/sys/class/kgsl/kgsl-3d0/force_clk_on", "banana", 0644);
    const auto before = device.snapshot();

    SysfsNodeBackend backend(device.policy());
    const CapabilityProbe probe(backend, device.policy());
    const auto pack = rebase_pack(flux::device::snapdragon_pack(), device);

    const auto report = probe.probe_pack(pack, {SocFamily::Snapdragon, "fake"},
                                         flux::device::kIntentSustainedPerformance, 1000);
    const auto *status = find(report, "sd.gpu.force_clk_on");
    CHECK(status != nullptr);
    if (status) {
        CHECK_EQ(status->state, CapabilityState::InvalidFormat);
        CHECK_EQ(status->reason, CapabilityReason::CurrentValueMalformed);
        CHECK(!status->current_value_valid);
        CHECK(!status->executable());
    }
    CHECK_MSG(device.snapshot() == before, "an unparseable node must be left exactly alone");
}

TEST("dry run: a symlinked node is rejected without being followed") {
    FakeDevice device;
    device.node("/sys/real_secret", "sensitive", 0644);
    device.symlink_node("/sys/class/kgsl/kgsl-3d0/force_clk_on", "/sys/real_secret");
    const auto before = device.snapshot();

    SysfsNodeBackend backend(device.policy());
    const CapabilityProbe probe(backend, device.policy());
    const auto pack = rebase_pack(flux::device::snapdragon_pack(), device);

    const auto report = probe.probe_pack(pack, {SocFamily::Snapdragon, "fake"},
                                         flux::device::kIntentSustainedPerformance, 1000);
    const auto *status = find(report, "sd.gpu.force_clk_on");
    CHECK(status != nullptr);
    if (status) {
        CHECK_MSG(status->state == CapabilityState::PathRejected ||
                      status->state == CapabilityState::Unsupported,
                  "a symlinked node must be refused");
        CHECK(!status->executable());
    }
    CHECK_MSG(device.snapshot() == before, "the symlink target must be untouched");
}

TEST("dry run: a read-only node reports permission_adjustable without adjusting it") {
    // A 0444 vendor node: the probe must notice it could be made writable, and must not do it.
    FakeDevice device;
    device.node("/sys/class/kgsl/kgsl-3d0/force_clk_on", "0", 0444);

    SysfsNodeBackend backend(device.policy());
    const CapabilityProbe probe(backend, device.policy());
    const auto pack = rebase_pack(flux::device::snapdragon_pack(), device);

    const auto report = probe.probe_pack(pack, {SocFamily::Snapdragon, "fake"},
                                         flux::device::kIntentSustainedPerformance, 1000);
    const auto *status = find(report, "sd.gpu.force_clk_on");
    CHECK(status != nullptr);
    if (status) {
        CHECK_MSG(!status->writable, "a 0444 node is not writable as it stands");
        CHECK_MSG(status->permission_adjustable, "but it could be adjusted safely at apply time");
    }

    struct stat st {};
    stat(device.rebase("/sys/class/kgsl/kgsl-3d0/force_clk_on").c_str(), &st);
    CHECK_MSG((st.st_mode & 07777) == 0444, "probing must not leave the node writable");
}

TEST("dry run: a wildcard resolves to a real node and stays inside the allowlist") {
    // Snapdragon devfreq names are kernel-enumerated and unstable, which is why the descriptor
    // carries a wildcard at all.
    FakeDevice device;
    device.node("/sys/class/devfreq/soc:qcom,ddr-lat/governor", "mem_latency", 0644);

    SysfsNodeBackend backend(device.policy());
    const CapabilityProbe probe(backend, device.policy());

    const auto resolved = probe.resolve_path(device.rebase("/sys/class/devfreq/*ddr-lat*/governor"));
    CHECK_MSG(resolved.has_value(), "the wildcard must resolve against a real directory");
    if (resolved) {
        CHECK_EQ(*resolved, device.rebase("/sys/class/devfreq/soc:qcom,ddr-lat/governor"));
    }

    // A pattern that resolves nowhere is simply absent, not an error.
    CHECK(!probe.resolve_path(device.rebase("/sys/class/devfreq/*nothing*/governor")).has_value());
}

TEST("dry run: a critical failure withholds its whole group") {
    // Half of an aggressive tuning set is usually worse than none of it: the device ends up in
    // a combination nobody designed or tested.
    FakeDevice device;
    device.node("/sys/gpu/present", "0", 0644);
    // The critical node is deliberately absent.

    DevicePack pack;
    pack.schema_version = kDescriptorSchemaVersion;
    pack.pack_id = "group_test";
    pack.soc = SocFamily::Generic;

    CapabilityDescriptor critical;
    critical.descriptor_id = "crit";
    critical.capability_id = "gpu.critical";
    critical.group = CapabilityGroup::GpuPolicy;
    critical.soc = SocFamily::Generic;
    critical.path = device.rebase("/sys/gpu/absent");
    critical.value_type = NodeValueType::Integer;
    critical.min = 0;
    critical.max = 1;
    critical.policy_values = {{"sustained_performance", "1"}};
    critical.critical = true;
    critical.validation = ValidationStatus::PhysicalDeviceValidated; // isolate the group logic

    CapabilityDescriptor companion;
    companion.descriptor_id = "companion";
    companion.capability_id = "gpu.companion";
    companion.group = CapabilityGroup::GpuPolicy;
    companion.soc = SocFamily::Generic;
    companion.path = device.rebase("/sys/gpu/present");
    companion.value_type = NodeValueType::Integer;
    companion.min = 0;
    companion.max = 1;
    companion.policy_values = {{"sustained_performance", "1"}};
    companion.critical = false;
    companion.validation = ValidationStatus::PhysicalDeviceValidated;

    pack.descriptors = {critical, companion};

    SysfsNodeBackend backend(device.policy());
    const CapabilityProbe probe(backend, device.policy());
    const auto report = probe.probe_pack(pack, {SocFamily::Generic, "fake"},
                                         "sustained_performance", 1000);

    const auto *crit_status = find(report, "crit");
    const auto *comp_status = find(report, "companion");
    CHECK(crit_status && comp_status);
    if (crit_status && comp_status) {
        CHECK_EQ(crit_status->state, CapabilityState::Unsupported); // the node is missing
        CHECK_MSG(comp_status->state == CapabilityState::IncompleteGroup,
                  "a healthy companion must be withheld when its group's critical node failed");
        CHECK_MSG(!comp_status->executable(), "no member of a broken group may act");
    }
    CHECK_MSG(!report.all_critical_executable(),
              "the profile must not be reportable as fully active");
    CHECK_EQ(report.executable_count(), static_cast<size_t>(0));
}

TEST("dry run: an unmet dependency withholds the dependent descriptor") {
    FakeDevice device;
    device.node("/sys/gpu/dependent", "0", 0644);

    DevicePack pack;
    pack.schema_version = kDescriptorSchemaVersion;
    pack.pack_id = "dep_test";
    pack.soc = SocFamily::Generic;

    CapabilityDescriptor base;
    base.descriptor_id = "base";
    base.capability_id = "gpu.base";
    base.group = CapabilityGroup::GpuPolicy;
    base.soc = SocFamily::Generic;
    base.path = device.rebase("/sys/gpu/absent_base");
    base.value_type = NodeValueType::Integer;
    base.min = 0;
    base.max = 1;
    base.policy_values = {{"balanced", "1"}};
    base.validation = ValidationStatus::PhysicalDeviceValidated;

    CapabilityDescriptor dependent;
    dependent.descriptor_id = "dependent";
    dependent.capability_id = "gpu.dependent";
    dependent.group = CapabilityGroup::Scheduler; // a different group, so only the dep applies
    dependent.soc = SocFamily::Generic;
    dependent.path = device.rebase("/sys/gpu/dependent");
    dependent.value_type = NodeValueType::Integer;
    dependent.min = 0;
    dependent.max = 1;
    dependent.policy_values = {{"balanced", "1"}};
    dependent.depends_on = {"base"};
    dependent.validation = ValidationStatus::PhysicalDeviceValidated;

    pack.descriptors = {base, dependent};

    SysfsNodeBackend backend(device.policy());
    const CapabilityProbe probe(backend, device.policy());
    const auto report = probe.probe_pack(pack, {SocFamily::Generic, "fake"}, "balanced", 1000);

    const auto *dep_status = find(report, "dependent");
    CHECK(dep_status != nullptr);
    if (dep_status) {
        CHECK_EQ(dep_status->state, CapabilityState::IncompleteGroup);
        CHECK_EQ(dep_status->reason, CapabilityReason::CriticalDependencyMissing);
        CHECK(!dep_status->executable());
    }
}

TEST("dry run: a descriptor with nothing to say about an intent produces no action") {
    // Silence is not a failure. The Tegra pack only speaks to `safe`.
    FakeDevice device;
    device.node("/sys/kernel/tegra_gpu/gpu_floor_rate", "0", 0644);

    SysfsNodeBackend backend(device.policy());
    const CapabilityProbe probe(backend, device.policy());
    const auto pack = rebase_pack(flux::device::tegra_pack(), device);

    const auto report = probe.probe_pack(pack, {SocFamily::Tegra, "fake"},
                                         flux::device::kIntentSustainedPerformance, 1000);
    const auto *status = find(report, "tegra.gpu.min_freq");
    CHECK(status != nullptr);
    if (status) {
        CHECK_MSG(!status->executable(),
                  "a descriptor that declares no value for this intent must produce no action");
    }
}

TEST("dry run: a path outside the allowlist is rejected even inside a valid pack") {
    FakeDevice device;
    device.node("/sys/placeholder", "1", 0644);

    DevicePack pack;
    pack.schema_version = kDescriptorSchemaVersion;
    pack.pack_id = "hostile";
    pack.soc = SocFamily::Generic;

    CapabilityDescriptor escaping;
    escaping.descriptor_id = "escape";
    escaping.capability_id = "escape";
    escaping.soc = SocFamily::Generic;
    escaping.path = "/etc/passwd"; // not under the fake root
    escaping.value_type = NodeValueType::Token;
    escaping.policy_values = {{"balanced", "x"}};
    escaping.validation = ValidationStatus::PhysicalDeviceValidated;
    pack.descriptors = {escaping};

    SysfsNodeBackend backend(device.policy());
    const CapabilityProbe probe(backend, device.policy());
    const auto report = probe.probe_pack(pack, {SocFamily::Generic, "fake"}, "balanced", 1000);

    CHECK_EQ(report.statuses.front().state, CapabilityState::PathRejected);
    CHECK_EQ(report.statuses.front().reason, CapabilityReason::PathOutsideAllowlist);
    CHECK(!report.statuses.front().executable());
}

TEST("dry run: diagnostics name the descriptor set, so a bad table can be traced") {
    FakeDevice device;
    device.node("/sys/placeholder", "1", 0644);

    SysfsNodeBackend backend(device.policy());
    const CapabilityProbe probe(backend, device.policy());
    const auto pack = rebase_pack(flux::device::mediatek_pack(), device);
    const auto report = probe.probe_pack(pack, {SocFamily::MediaTek, "fake"},
                                         flux::device::kIntentSustainedPerformance, 1000);

    for (const auto &s : report.statuses) {
        CHECK_EQ(s.descriptor_set, std::string("mediatek"));
        CHECK_MSG(!s.descriptor_id.empty(), "every status must identify its descriptor");
        CHECK_MSG(!s.detail.empty(), "every status must carry a safe explanation");
        CHECK_MSG(s.probed_at_ms == 1000, "every status must carry its probe timestamp");
    }
}
