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

#include "RuntimeTuning.hpp"

#include <algorithm>

namespace flux::execution {

const char *migration_outcome_name(MigrationOutcome outcome) {
    switch (outcome) {
        case MigrationOutcome::Migrated: return "migrated";
        case MigrationOutcome::Aliased: return "aliased";
        case MigrationOutcome::Ignored: return "ignored";
        case MigrationOutcome::Rejected: return "rejected";
    }
    return "unknown";
}

namespace {

/// The governors Flux is willing to select. Anything else is refused rather than passed through:
/// a governor Flux has not reasoned about is one whose behaviour it cannot predict, and the
/// config file is user-writable — "cpu_governor": "../../etc/passwd" must not become a value
/// this code hands to a descriptor.
const std::set<std::string> &known_governors() {
    static const std::set<std::string> governors{"schedutil", "performance", "powersave",
                                                 "walt",      "interactive", "conservative",
                                                 "ondemand",  "userspace"};
    return governors;
}

/// A conservative governor choice is always safe to honour; an aggressive one is not.
bool governor_is_conservative(const std::string &governor) {
    return governor != "performance";
}

} // namespace

bool RuntimeTuning::suppresses(const CapabilityDescriptor &descriptor) const {
    if (suppressed_capability_ids.count(descriptor.capability_id) > 0) return true;
    if (suppressed_capability_ids.count(descriptor.descriptor_id) > 0) return true;
    return suppressed_groups.count(descriptor.group) > 0;
}

RuntimeTuning migrate_legacy_config(const LegacyConfigInput &input) {
    RuntimeTuning tuning;
    const auto note = [&](std::string key, MigrationOutcome outcome, std::string detail) {
        tuning.notes.push_back({std::move(key), outcome, std::move(detail)});
    };

    // --- preferences.disable_tweaks -> the master gate -----------------------
    // The legacy dispatchers each opened with `if (disable_tweaks) return;`. That was the only
    // place the setting was enforced, so removing them removed the setting. Honouring a user's
    // "do not touch my device" is not a feature to reimplement later.
    tuning.tweaks_enabled = !input.disable_tweaks;
    note("preferences.disable_tweaks", MigrationOutcome::Migrated,
         input.disable_tweaks ? "tweaks are disabled: Flux will apply nothing and restore what "
                                "it changed"
                              : "tweaks are enabled");

    // --- cpu_governor.balance / .powersave -> descriptor values -------------
    // Legacy exported these as FLUX_BALANCED_CPUGOV / FLUX_POWERSAVE_CPUGOV and the shell wrote
    // whatever they contained. V2 puts them into the generic pack's policy values, where they
    // are validated against the descriptor's allowlist and verified by read-back like any other
    // value — so an unusable governor now fails visibly instead of silently.
    if (!input.balanced_governor.empty()) {
        if (known_governors().count(input.balanced_governor) == 0) {
            note("cpu_governor.balance", MigrationOutcome::Rejected,
                 "'" + input.balanced_governor +
                     "' is not a governor Flux selects; keeping " + tuning.balanced_governor);
        } else {
            tuning.balanced_governor = input.balanced_governor;
            note("cpu_governor.balance", MigrationOutcome::Migrated,
                 "balanced and interactive behaviour will use '" + tuning.balanced_governor + "'");
        }
    }
    if (!input.powersave_governor.empty()) {
        if (known_governors().count(input.powersave_governor) == 0) {
            note("cpu_governor.powersave", MigrationOutcome::Rejected,
                 "'" + input.powersave_governor +
                     "' is not a governor Flux selects; keeping " + tuning.powersave_governor);
        } else if (!governor_is_conservative(input.powersave_governor)) {
            // "powersave: performance" is a contradiction, and the legacy shell would have
            // written it without comment. Refusing is not a limitation; it is the setting
            // meaning what it says.
            note("cpu_governor.powersave", MigrationOutcome::Rejected,
                 "the power-save governor may not be 'performance'; keeping " +
                     tuning.powersave_governor);
        } else {
            tuning.powersave_governor = input.powersave_governor;
            note("cpu_governor.powersave", MigrationOutcome::Migrated,
                 "power-save behaviour will use '" + tuning.powersave_governor + "'");
        }
    }

    // --- device mitigation items -> suppressed capabilities -----------------
    // Each item was an FLUX_* environment variable the shell branched on. In V2 the item makes
    // Flux decline to offer the capability at all, which is stronger: the planner never plans
    // it, so there is no step left that could write it by mistake.
    for (const auto &item : input.mitigation_items) {
        if (item == "DISABLE_DDR_TWEAK") {
            tuning.suppressed_groups.insert(CapabilityGroup::Memory);
            note(item, MigrationOutcome::Migrated,
                 "memory/DDR capabilities are suppressed on this device");
        } else if (item == "NO_PERFORMANCE_CPUGOV") {
            // Real hardware safety: some kernels hang or reboot on the performance governor.
            tuning.allow_performance_governor = false;
            note(item, MigrationOutcome::Migrated,
                 "the performance governor is suppressed; sustained performance will use '" +
                     tuning.balanced_governor + "'");
        } else if (item == "QCOM_NO_GPU_POWERSAVE") {
            tuning.suppressed_capability_ids.insert("qcom.gpu.min_pwrlevel");
            note(item, MigrationOutcome::Migrated,
                 "the Snapdragon GPU power-save capability is suppressed on this device");
        } else {
            // An unknown item is not silently dropped and not guessed at. Guessing which
            // capability an unrecognised name meant is how a mitigation stops mitigating.
            note(item, MigrationOutcome::Ignored,
                 "unrecognised mitigation item; no capability was suppressed for it");
        }
    }

    // Deterministic notes, so a migration diffs cleanly and a test can assert order.
    std::stable_sort(tuning.notes.begin(), tuning.notes.end(),
                     [](const MigrationNote &a, const MigrationNote &b) { return a.key < b.key; });
    return tuning;
}

std::vector<DevicePack> apply_tuning(std::vector<DevicePack> packs, const RuntimeTuning &tuning) {
    for (auto &pack : packs) {
        std::vector<CapabilityDescriptor> kept;
        kept.reserve(pack.descriptors.size());

        for (auto &descriptor : pack.descriptors) {
            if (tuning.suppresses(descriptor)) continue; // not a candidate at all

            // Substitute the user's governor choices into the values the descriptor offers.
            // Only where the descriptor already has something to say: this replaces a value, it
            // never invents a capability the pack did not declare.
            if (descriptor.group == CapabilityGroup::CpuPolicy) {
                const auto set_value = [&](const char *key, const std::string &value) {
                    auto it = descriptor.policy_values.find(key);
                    if (it == descriptor.policy_values.end()) return;
                    // Respect the descriptor's own allowlist. A governor the node does not
                    // accept must not be written just because the user typed it.
                    if (!descriptor.allowed.empty() &&
                        std::find(descriptor.allowed.begin(), descriptor.allowed.end(), value) ==
                            descriptor.allowed.end()) {
                        return;
                    }
                    it->second = value;
                };

                set_value("balanced", tuning.balanced_governor);
                set_value("constrained_performance", tuning.balanced_governor);
                set_value("power_save", tuning.powersave_governor);

                if (!tuning.allow_performance_governor) {
                    // The device cannot take the performance governor. Fall back to the
                    // balanced choice rather than dropping the capability: a slower phone is
                    // the point of the mitigation, and no capability at all would mean the
                    // profile does nothing.
                    set_value("sustained_performance", tuning.balanced_governor);
                }
            }

            kept.push_back(std::move(descriptor));
        }
        pack.descriptors = std::move(kept);
    }
    return packs;
}

} // namespace flux::execution
