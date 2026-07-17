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

// Legacy configuration -> V2 semantics.
//
// These settings used to reach the device as FLUX_* environment variables that a shell script
// branched on. Deleting the shell deleted their only readers, so each one here is a regression
// test as much as a unit test: the question is not "does the mapping work" but "does the setting
// still do what the user was promised".

#include "TestFramework.hpp"

#include "DevicePacks.hpp"
#include "RuntimeTuning.hpp"

#include <algorithm>
#include <string>

using namespace flux::execution;

namespace {

const MigrationNote *note_for(const RuntimeTuning &tuning, const std::string &key) {
    const auto it = std::find_if(tuning.notes.begin(), tuning.notes.end(),
                                 [&](const MigrationNote &n) { return n.key == key; });
    return it == tuning.notes.end() ? nullptr : &*it;
}

/// The value the generic pack would write for one intent key, after tuning.
std::string governor_for(const RuntimeTuning &tuning, const char *intent_key) {
    const auto packs = apply_tuning({flux::device::generic_pack()}, tuning);
    for (const auto &descriptor : packs.front().descriptors) {
        if (descriptor.group != CapabilityGroup::CpuPolicy) continue;
        const auto it = descriptor.policy_values.find(intent_key);
        if (it != descriptor.policy_values.end()) return it->second;
    }
    return "<none>";
}

} // namespace

// --- the master switch -----------------------------------------------------

TEST("migration: disable_tweaks turns the master gate off") {
    // The regression this whole file exists for. Every legacy dispatcher began with
    // `if (disable_tweaks) return;`, and that was the only enforcement anywhere.
    LegacyConfigInput input;
    input.disable_tweaks = true;

    const auto tuning = migrate_legacy_config(input);

    CHECK_MSG(!tuning.tweaks_enabled,
              "a user who turned tweaks off must not have tweaks applied");
    const auto *note = note_for(tuning, "preferences.disable_tweaks");
    CHECK(note != nullptr);
    CHECK_EQ(note->outcome, MigrationOutcome::Migrated);
}

TEST("migration: tweaks are enabled by default") {
    const auto tuning = migrate_legacy_config({});
    CHECK(tuning.tweaks_enabled);
}

// --- governors -------------------------------------------------------------

TEST("migration: the user's balanced governor reaches the descriptor") {
    LegacyConfigInput input;
    input.balanced_governor = "walt";

    const auto tuning = migrate_legacy_config(input);

    CHECK_EQ(tuning.balanced_governor, std::string("walt"));
    CHECK_MSG(governor_for(tuning, "balanced") == "walt",
              "the setting must reach the value the engine will write, got: " +
                  governor_for(tuning, "balanced"));
    CHECK_EQ(note_for(tuning, "cpu_governor.balance")->outcome, MigrationOutcome::Migrated);
}

TEST("migration: the user's powersave governor reaches the descriptor") {
    // schedutil, not something exotic: the value has to be one the generic descriptor lists as
    // accepted by the node, which is the allowlist rule tested separately below.
    LegacyConfigInput input;
    input.powersave_governor = "schedutil";

    const auto tuning = migrate_legacy_config(input);

    CHECK_EQ(tuning.powersave_governor, std::string("schedutil"));
    CHECK_EQ(governor_for(tuning, "power_save"), std::string("schedutil"));
}

TEST("migration: power save is a distinct intent from a thermal fallback") {
    // Collapsing them meant a power-save profile applied the balanced governor, because "safe"
    // is what a thermal response wants. The legacy script had a separate powersave governor.
    const auto tuning = migrate_legacy_config({});

    CHECK_MSG(governor_for(tuning, "power_save") == "powersave",
              "power save must ask for a slower CPU, got: " + governor_for(tuning, "power_save"));
    CHECK_MSG(governor_for(tuning, "safe") == "schedutil",
              "a thermal fallback wants the kernel's balanced behaviour, not a power floor");
}

TEST("migration: an unknown governor is rejected, not passed through") {
    // The config file is user-writable. A governor Flux has not reasoned about must not become
    // a value it hands to a descriptor.
    LegacyConfigInput input;
    input.balanced_governor = "../../etc/passwd";

    const auto tuning = migrate_legacy_config(input);

    CHECK_MSG(tuning.balanced_governor == "schedutil", "the default must survive a bad value");
    CHECK_EQ(note_for(tuning, "cpu_governor.balance")->outcome, MigrationOutcome::Rejected);
    CHECK_MSG(governor_for(tuning, "balanced") == "schedutil",
              "a rejected value must never reach a descriptor");
}

TEST("migration: a powersave governor of 'performance' is refused") {
    LegacyConfigInput input;
    input.powersave_governor = "performance";

    const auto tuning = migrate_legacy_config(input);

    CHECK_EQ(tuning.powersave_governor, std::string("powersave"));
    CHECK_EQ(note_for(tuning, "cpu_governor.powersave")->outcome, MigrationOutcome::Rejected);
}

TEST("migration: a governor the node does not accept is not written") {
    // The descriptor's own allowlist still wins. "ondemand" is a real governor Flux is willing
    // to select, but the generic descriptor does not list it as accepted by the node.
    LegacyConfigInput input;
    input.balanced_governor = "conservative";

    const auto tuning = migrate_legacy_config(input);

    CHECK_EQ(tuning.balanced_governor, std::string("conservative"));
    CHECK_MSG(governor_for(tuning, "balanced") == "schedutil",
              "the descriptor's allowlist outranks the user's choice: writing a value the node "
              "rejects would fail verification and withdraw the profile");
}

// --- mitigation items ------------------------------------------------------

TEST("migration: NO_PERFORMANCE_CPUGOV keeps the performance governor off the device") {
    // Real hardware safety: some kernels hang or reboot on it. The legacy shell honoured this,
    // so V2 must, or the cutover made those devices less safe than before.
    LegacyConfigInput input;
    input.mitigation_items = {"NO_PERFORMANCE_CPUGOV"};

    const auto tuning = migrate_legacy_config(input);

    CHECK(!tuning.allow_performance_governor);
    CHECK_MSG(governor_for(tuning, "sustained_performance") == "schedutil",
              "a device that cannot take the performance governor must not be given it, got: " +
                  governor_for(tuning, "sustained_performance"));
}

TEST("migration: DISABLE_DDR_TWEAK suppresses the memory group entirely") {
    LegacyConfigInput input;
    input.mitigation_items = {"DISABLE_DDR_TWEAK"};

    const auto tuning = migrate_legacy_config(input);

    CHECK(tuning.suppressed_groups.count(CapabilityGroup::Memory) > 0);

    CapabilityDescriptor memory_node;
    memory_node.group = CapabilityGroup::Memory;
    memory_node.capability_id = "ddr.boost";
    CHECK_MSG(tuning.suppresses(memory_node), "a suppressed group must suppress its members");
}

TEST("migration: a suppressed capability is removed from the pack, not merely flagged") {
    // Suppression by removal means the planner never sees it. A candidate that something
    // downstream is trusted to skip is a candidate that will eventually be written.
    RuntimeTuning tuning;
    tuning.suppressed_groups.insert(CapabilityGroup::CpuPolicy);

    const auto packs = apply_tuning({flux::device::generic_pack()}, tuning);

    const bool any_cpu = std::any_of(packs.front().descriptors.begin(),
                                     packs.front().descriptors.end(),
                                     [](const CapabilityDescriptor &d) {
                                         return d.group == CapabilityGroup::CpuPolicy;
                                     });
    CHECK_MSG(!any_cpu, "a suppressed capability must not be a candidate at all");
}

TEST("migration: an unrecognised mitigation item is reported, never guessed at") {
    LegacyConfigInput input;
    input.mitigation_items = {"SOME_FUTURE_ITEM"};

    const auto tuning = migrate_legacy_config(input);

    const auto *note = note_for(tuning, "SOME_FUTURE_ITEM");
    CHECK(note != nullptr);
    CHECK_EQ(note->outcome, MigrationOutcome::Ignored);
    CHECK_MSG(tuning.suppressed_groups.empty() && tuning.suppressed_capability_ids.empty(),
              "guessing which capability an unknown name meant is how a mitigation stops "
              "mitigating");
}

// --- properties ------------------------------------------------------------

TEST("migration: the same configuration always migrates to the same result") {
    LegacyConfigInput input;
    input.balanced_governor = "walt";
    input.mitigation_items = {"DISABLE_DDR_TWEAK", "NO_PERFORMANCE_CPUGOV", "UNKNOWN_ONE"};

    const auto first = migrate_legacy_config(input);
    const auto second = migrate_legacy_config(input);

    CHECK_EQ(first.notes.size(), second.notes.size());
    for (size_t i = 0; i < first.notes.size(); ++i) {
        CHECK_EQ(first.notes[i].key, second.notes[i].key);
        CHECK_EQ(first.notes[i].outcome, second.notes[i].outcome);
    }
    CHECK_EQ(first.balanced_governor, second.balanced_governor);
}

TEST("migration: an empty configuration is safe, not empty-handed") {
    // A fresh install, or a config file that failed to parse and left defaults.
    const auto tuning = migrate_legacy_config({});
    const auto packs = apply_tuning({flux::device::generic_pack()}, tuning);

    CHECK(tuning.tweaks_enabled);
    CHECK_MSG(!packs.front().descriptors.empty(),
              "defaults must still leave the generic fallback usable");
    CHECK_EQ(governor_for(tuning, "sustained_performance"), std::string("performance"));
}

TEST("migration: tuning re-derives from the original packs, so a setting can be undone") {
    // Applying tuning to an already-tuned set would make every suppression permanent: a user
    // who turned a mitigation off could not turn it back on without restarting the daemon.
    RuntimeTuning suppressing;
    suppressing.suppressed_groups.insert(CapabilityGroup::CpuPolicy);
    const auto base = flux::device::generic_pack();

    const auto suppressed = apply_tuning({base}, suppressing);
    CHECK(suppressed.front().descriptors.empty());

    const auto restored = apply_tuning({base}, RuntimeTuning{});
    CHECK_MSG(!restored.front().descriptors.empty(),
              "re-applying default tuning to the original packs must bring the capability back");
}
