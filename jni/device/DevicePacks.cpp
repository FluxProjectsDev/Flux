/*
 * Copyright (C) 2024-2026 Rem01Gaming
 * Copyright (C) 2024-2026 FebriCahyaa
 *
 * The per-SoC tuning knowledge expressed by these packs is derived from Encore Tweaks
 * (https://github.com/Rem01Gaming/encore), by way of the per-SoC blocks of
 * scripts/flux_profiler.sh. Modified by the Flux project: re-expressed as declarative data,
 * re-scoped, and gated behind runtime capability probing.
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

#include "DevicePacks.hpp"

namespace flux::device {

using namespace flux::execution;

namespace {

/// Attribution carried by every derived pack. The validator refuses a DerivedEncore pack that
/// does not set this, so the obligation cannot be dropped by forgetting.
constexpr const char *kEncoreAttribution =
    "Device tuning knowledge derived from Encore Tweaks "
    "(https://github.com/Rem01Gaming/encore), Copyright (C) 2024-2026 Rem01Gaming, "
    "Apache-2.0. Re-expressed as declarative data and modified by the Flux project.";

/// Every derived descriptor starts here. Only real hardware moves it.
constexpr ValidationStatus kUnvalidated = ValidationStatus::PhysicalDeviceRequired;

CapabilityDescriptor derived(std::string descriptor_id, std::string capability_id,
                             CapabilityGroup group, SocFamily soc, std::string path) {
    CapabilityDescriptor d;
    d.schema_version = kDescriptorSchemaVersion;
    d.descriptor_id = std::move(descriptor_id);
    d.capability_id = std::move(capability_id);
    d.group = group;
    d.soc = soc;
    d.path = std::move(path);
    d.provenance = Provenance::DerivedEncore;
    d.validation = kUnvalidated;
    // Vendor tuning is optional by default. A device missing one of these is a device that
    // simply does not have that knob; refusing to run because of it would be wrong. Marking a
    // vendor node critical means "an aggressive profile is unsafe without it", and that claim
    // needs hardware evidence before anyone makes it.
    d.critical = false;
    return d;
}

CapabilityDescriptor token_node(CapabilityDescriptor d, std::vector<std::string> allowed) {
    d.value_type = allowed.empty() ? NodeValueType::Token : NodeValueType::Enum;
    d.allowed = std::move(allowed);
    return d;
}

CapabilityDescriptor int_node(CapabilityDescriptor d, long min, long max) {
    d.value_type = NodeValueType::Integer;
    d.min = min;
    d.max = max;
    return d;
}

} // namespace

// ---------------------------------------------------------------------------
// Generic — standard kernel interfaces, present regardless of SoC.
//
// This pack is Flux-authored (Category A knowledge): cpufreq's scaling_governor and the
// /proc/sys/kernel scheduler tunables are documented upstream Linux interfaces, not Encore's
// contribution. It carries no Encore attribution because none is owed.
// ---------------------------------------------------------------------------
DevicePack generic_pack() {
    DevicePack pack;
    pack.schema_version = kDescriptorSchemaVersion;
    pack.pack_id = "generic";
    pack.soc = SocFamily::Generic;
    pack.provenance = Provenance::FluxAuthored;

    // cpufreq governors, documented at Documentation/admin-guide/pm/cpufreq.rst. The allowlist
    // is the set Flux is willing to select, not everything a kernel might offer: a governor
    // outside it is one whose behaviour Flux has not reasoned about.
    for (int policy : {0, 4, 7}) {
        auto d = derived("generic.cpu.policy" + std::to_string(policy) + ".governor",
                         "cpu.policy" + std::to_string(policy) + ".scaling_governor",
                         CapabilityGroup::CpuPolicy, SocFamily::Generic,
                         "/sys/devices/system/cpu/cpufreq/policy" + std::to_string(policy) +
                             "/scaling_governor");
        d.provenance = Provenance::FluxAuthored;
        d = token_node(std::move(d), {"schedutil", "performance", "powersave", "walt", "interactive"});
        d.policy_values = {
            {kIntentSustainedPerformance, "performance"},
            {kIntentConstrainedPerformance, "schedutil"},
            {kIntentBalanced, "schedutil"},
            {kIntentSafe, "schedutil"},
        };
        d.read_back = ReadBackStrategy::Exact;
        d.rollback = RollbackStrategy::RestoreOriginal;
        d.order_group = 10;
        pack.descriptors.push_back(std::move(d));
    }

    return pack;
}

// ---------------------------------------------------------------------------
// MediaTek — /proc/ppm, /proc/cpufreq, /proc/gpufreq*, /proc/perfmgr.
// Nodes observed in mediatek_performance() / mediatek_normal() / mediatek_powersave().
// ---------------------------------------------------------------------------
DevicePack mediatek_pack() {
    DevicePack pack;
    pack.schema_version = kDescriptorSchemaVersion;
    pack.pack_id = "mediatek";
    pack.soc = SocFamily::MediaTek;
    pack.provenance = Provenance::DerivedEncore;
    pack.attribution = kEncoreAttribution;

    {
        // CCI mode. The legacy values are opaque small integers; the range is declared
        // conservatively and the meaning is unverified until hardware says otherwise.
        auto d = int_node(derived("mtk.cpu.cci_mode", "cpu.cci_mode", CapabilityGroup::CpuPolicy,
                                  SocFamily::MediaTek, "/proc/cpufreq/cpufreq_cci_mode"),
                          0, 4);
        d.policy_values = {{kIntentSustainedPerformance, "1"}, {kIntentBalanced, "0"},
                           {kIntentSafe, "0"}};
        d.read_back = ReadBackStrategy::Numeric;
        d.order_group = 20;
        pack.descriptors.push_back(std::move(d));
    }
    {
        auto d = int_node(derived("mtk.cpu.power_mode", "cpu.power_mode", CapabilityGroup::PowerMode,
                                  SocFamily::MediaTek, "/proc/cpufreq/cpufreq_power_mode"),
                          0, 3);
        d.policy_values = {{kIntentSustainedPerformance, "1"}, {kIntentBalanced, "0"},
                           {kIntentSafe, "0"}};
        d.read_back = ReadBackStrategy::Numeric;
        d.order_group = 20;
        pack.descriptors.push_back(std::move(d));
    }
    {
        auto d = int_node(derived("mtk.gpu.power_limited", "gpu.power_limited",
                                  CapabilityGroup::GpuPolicy, SocFamily::MediaTek,
                                  "/proc/gpufreq/gpufreq_power_limited"),
                          0, 1);
        d.policy_values = {{kIntentSustainedPerformance, "1"}, {kIntentBalanced, "0"},
                           {kIntentSafe, "0"}};
        d.read_back = ReadBackStrategy::Numeric;
        d.order_group = 30;
        pack.descriptors.push_back(std::move(d));
    }
    {
        // PPM hard user limit. Read-back on PPM nodes returns a formatted table rather than the
        // written value, so Exact would always mismatch. Declared Contains, and therefore
        // optional: a node Flux cannot verify exactly is one it must not treat as critical.
        auto d = derived("mtk.ppm.hard_userlimit_cpu_freq", "cpu.ppm.hard_userlimit",
                         CapabilityGroup::CpuPolicy, SocFamily::MediaTek,
                         "/proc/ppm/policy/hard_userlimit_cpu_freq");
        d.value_type = NodeValueType::Token;
        d.policy_values = {{kIntentSafe, "0 0"}};
        d.read_back = ReadBackStrategy::Contains;
        d.rollback = RollbackStrategy::RestoreOriginal;
        d.order_group = 15;
        pack.descriptors.push_back(std::move(d));
    }

    return pack;
}

// ---------------------------------------------------------------------------
// Snapdragon — kgsl (GPU) and devfreq. Nodes observed in snapdragon_performance().
//
// The devfreq names are enumerated by the kernel and are not stable across devices, hence the
// wildcard segment; CapabilityProbe resolves it and re-checks the result against the allowlist.
// ---------------------------------------------------------------------------
DevicePack snapdragon_pack() {
    DevicePack pack;
    pack.schema_version = kDescriptorSchemaVersion;
    pack.pack_id = "snapdragon";
    pack.soc = SocFamily::Snapdragon;
    pack.provenance = Provenance::DerivedEncore;
    pack.attribution = kEncoreAttribution;

    {
        auto d = int_node(derived("sd.gpu.force_clk_on", "gpu.force_clk_on",
                                  CapabilityGroup::GpuPolicy, SocFamily::Snapdragon,
                                  "/sys/class/kgsl/kgsl-3d0/force_clk_on"),
                          0, 1);
        d.policy_values = {{kIntentSustainedPerformance, "1"}, {kIntentBalanced, "0"},
                           {kIntentSafe, "0"}};
        d.read_back = ReadBackStrategy::Numeric;
        d.order_group = 30;
        pack.descriptors.push_back(std::move(d));
    }
    {
        auto d = int_node(derived("sd.gpu.bus_split", "gpu.bus_split", CapabilityGroup::GpuPolicy,
                                  SocFamily::Snapdragon, "/sys/class/kgsl/kgsl-3d0/bus_split"),
                          0, 1);
        d.policy_values = {{kIntentSustainedPerformance, "0"}, {kIntentBalanced, "1"},
                           {kIntentSafe, "1"}};
        d.read_back = ReadBackStrategy::Numeric;
        d.order_group = 30;
        pack.descriptors.push_back(std::move(d));
    }
    {
        auto d = token_node(derived("sd.ddr.governor", "memory.ddr_lat.governor",
                                    CapabilityGroup::Memory, SocFamily::Snapdragon,
                                    "/sys/class/devfreq/*ddr-lat*/governor"),
                            {"performance", "powersave", "mem_latency", "simple_ondemand"});
        d.policy_values = {{kIntentSustainedPerformance, "performance"},
                           {kIntentBalanced, "mem_latency"}};
        d.read_back = ReadBackStrategy::Exact;
        d.order_group = 40;
        pack.descriptors.push_back(std::move(d));
    }

    return pack;
}

// ---------------------------------------------------------------------------
// Exynos — /sys/kernel/gpu and devfreq_mif. Nodes observed in exynos_performance().
// ---------------------------------------------------------------------------
DevicePack exynos_pack() {
    DevicePack pack;
    pack.schema_version = kDescriptorSchemaVersion;
    pack.pack_id = "exynos";
    pack.soc = SocFamily::Exynos;
    pack.provenance = Provenance::DerivedEncore;
    pack.attribution = kEncoreAttribution;

    {
        auto d = int_node(derived("exynos.gpu.highspeed_clock", "gpu.highspeed_clock",
                                  CapabilityGroup::GpuPolicy, SocFamily::Exynos,
                                  "/sys/kernel/gpu/gpu_highspeed_clock"),
                          0, 2000000);
        d.policy_values = {{kIntentSustainedPerformance, "0"}};
        d.read_back = ReadBackStrategy::Numeric;
        d.order_group = 30;
        pack.descriptors.push_back(std::move(d));
    }
    {
        auto d = token_node(derived("exynos.mif.governor", "memory.mif.governor",
                                    CapabilityGroup::Memory, SocFamily::Exynos,
                                    "/sys/class/devfreq/*devfreq_mif*/governor"),
                            {"performance", "powersave", "simple_ondemand"});
        d.policy_values = {{kIntentSustainedPerformance, "performance"},
                           {kIntentBalanced, "simple_ondemand"}};
        d.read_back = ReadBackStrategy::Exact;
        d.order_group = 40;
        pack.descriptors.push_back(std::move(d));
    }

    return pack;
}

// ---------------------------------------------------------------------------
// Unisoc — devfreq only. unisoc_performance() touches very little; the pack is small because
// the evidence is small, not because it was trimmed.
// ---------------------------------------------------------------------------
DevicePack unisoc_pack() {
    DevicePack pack;
    pack.schema_version = kDescriptorSchemaVersion;
    pack.pack_id = "unisoc";
    pack.soc = SocFamily::Unisoc;
    pack.provenance = Provenance::DerivedEncore;
    pack.attribution = kEncoreAttribution;

    auto d = token_node(derived("unisoc.gpu.governor", "gpu.governor", CapabilityGroup::GpuPolicy,
                                SocFamily::Unisoc, "/sys/class/devfreq/gpufreq/governor"),
                        {"performance", "powersave", "simple_ondemand"});
    d.policy_values = {{kIntentSustainedPerformance, "performance"},
                       {kIntentBalanced, "simple_ondemand"}};
    d.read_back = ReadBackStrategy::Exact;
    d.order_group = 30;
    pack.descriptors.push_back(std::move(d));

    return pack;
}

// ---------------------------------------------------------------------------
// Tensor — devfreq_mif, as observed in tensor_performance().
// ---------------------------------------------------------------------------
DevicePack tensor_pack() {
    DevicePack pack;
    pack.schema_version = kDescriptorSchemaVersion;
    pack.pack_id = "tensor";
    pack.soc = SocFamily::Tensor;
    pack.provenance = Provenance::DerivedEncore;
    pack.attribution = kEncoreAttribution;

    auto d = token_node(derived("tensor.mif.governor", "memory.mif.governor",
                                CapabilityGroup::Memory, SocFamily::Tensor,
                                "/sys/class/devfreq/*devfreq_mif*/governor"),
                        {"performance", "powersave", "simple_ondemand"});
    d.policy_values = {{kIntentSustainedPerformance, "performance"},
                       {kIntentBalanced, "simple_ondemand"}};
    d.read_back = ReadBackStrategy::Exact;
    d.order_group = 40;
    pack.descriptors.push_back(std::move(d));

    return pack;
}

// ---------------------------------------------------------------------------
// Tegra — /sys/kernel/tegra_gpu, as observed in tegra_performance().
// ---------------------------------------------------------------------------
DevicePack tegra_pack() {
    DevicePack pack;
    pack.schema_version = kDescriptorSchemaVersion;
    pack.pack_id = "tegra";
    pack.soc = SocFamily::Tegra;
    pack.provenance = Provenance::DerivedEncore;
    pack.attribution = kEncoreAttribution;

    auto d = int_node(derived("tegra.gpu.min_freq", "gpu.min_freq", CapabilityGroup::GpuPolicy,
                              SocFamily::Tegra, "/sys/kernel/tegra_gpu/gpu_floor_rate"),
                      0, 2000000000);
    d.policy_values = {{kIntentSafe, "0"}};
    d.read_back = ReadBackStrategy::Numeric;
    d.order_group = 30;
    pack.descriptors.push_back(std::move(d));

    return pack;
}

std::vector<DevicePack> all_packs() {
    return {generic_pack(),  mediatek_pack(), snapdragon_pack(), exynos_pack(),
            unisoc_pack(),   tensor_pack(),   tegra_pack()};
}

std::vector<DevicePack> packs_for(SocFamily soc) {
    std::vector<DevicePack> out{generic_pack()};
    switch (soc) {
        case SocFamily::MediaTek: out.push_back(mediatek_pack()); break;
        case SocFamily::Snapdragon: out.push_back(snapdragon_pack()); break;
        case SocFamily::Exynos: out.push_back(exynos_pack()); break;
        case SocFamily::Unisoc: out.push_back(unisoc_pack()); break;
        case SocFamily::Tensor: out.push_back(tensor_pack()); break;
        case SocFamily::Tegra: out.push_back(tegra_pack()); break;
        case SocFamily::Unknown:
        case SocFamily::Generic:
            // An unrecognised SoC gets the generic pack only. Guessing a family would be the
            // one mistake this whole design exists to prevent.
            break;
    }
    return out;
}

} // namespace flux::device
