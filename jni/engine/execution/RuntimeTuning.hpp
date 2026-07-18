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

#include <set>
#include <string>
#include <vector>

#include "DeviceDescriptor.hpp"

/**
 * @file RuntimeTuning.hpp
 * @brief Migrates legacy configuration into V2 semantics. Flux-owned (Category A).
 *
 * ## Why this exists
 *
 * The legacy profiler read its settings from the environment: the daemon exported
 * `FLUX_BALANCED_CPUGOV`, `FLUX_DISABLE_DDR_TWEAK` and friends, and the shell branched on them.
 * When the shell applier and its dispatchers were deleted, those exports went with them — and
 * the settings behind them were left with no reader at all. The WebUI still offered them; the
 * config file still stored them; nothing acted on them.
 *
 * That is a silent regression, and the worst kind: a user who turned tweaks *off* would have had
 * them applied anyway, because `disable_tweaks` was only ever checked at the top of the very
 * functions that were removed.
 *
 * This is the migration boundary. It turns stored configuration into semantic V2 inputs:
 *
 *     stored config + device mitigation items
 *       -> RuntimeTuning (semantic)
 *       -> descriptor values / suppression / a hard gate
 *       -> PolicyIntent -> ExecutionPlan
 *
 * It parses old keys. It never revives the old write path: there is no shell here, no path, no
 * node, and nothing in this file can execute anything. A migrated key becomes a *value in a
 * descriptor* or a *capability that is not offered*, and then the normal engine rules apply —
 * probe, plan, verify. A migrated setting still has to survive verification like anything else.
 */
namespace flux::execution {

/** What a legacy key turned into, for the diagnostics record. */
enum class MigrationOutcome {
    Migrated,   ///< the key now drives a V2 semantic input
    Aliased,    ///< the key is accepted as a synonym for a V2 setting
    Ignored,    ///< parsed, understood, and deliberately not acted on
    Rejected,   ///< the value is unsafe or malformed; the default is used instead
};

const char *migration_outcome_name(MigrationOutcome outcome);

/** One decision, so a user can be told what happened to a setting rather than guessing. */
struct MigrationNote {
    std::string key;
    MigrationOutcome outcome = MigrationOutcome::Ignored;
    std::string detail;
};

/**
 * @brief The semantic result of migrating stored configuration.
 *
 * Deliberately expressed as *outcomes and constraints*, not as paths or commands. Nothing here
 * says which node to write; that stays the descriptors' job.
 */
struct RuntimeTuning {
    /// The user's master switch. When false Flux applies nothing and restores what it changed.
    /// This was `preferences.disable_tweaks`, inverted: the legacy dispatchers each began with
    /// `if (disable_tweaks) return;`, and deleting them deleted the only enforcement of it.
    bool tweaks_enabled = true;

    /// The governor the user chose for each behaviour. Legacy exported these as
    /// FLUX_BALANCED_CPUGOV / FLUX_POWERSAVE_CPUGOV for the shell to read.
    std::string balanced_governor = "schedutil";
    std::string powersave_governor = "powersave";

    /// From the device-mitigation items. Each was an `FLUX_*` env var the shell branched on;
    /// each is now a capability Flux declines to offer, which the planner then never plans.
    std::set<std::string> suppressed_capability_ids;
    std::set<CapabilityGroup> suppressed_groups;

    /// NO_PERFORMANCE_CPUGOV: some kernels hang or reboot on the performance governor. The
    /// legacy shell honoured this; V2 must too, or the cutover made those devices less safe.
    bool allow_performance_governor = true;

    std::vector<MigrationNote> notes;

    [[nodiscard]] bool suppresses(const CapabilityDescriptor &descriptor) const;
};

/** The stored settings this migration understands, decoupled from the config store's types. */
struct LegacyConfigInput {
    bool disable_tweaks = false;
    std::string balanced_governor;  ///< empty means "not set"
    std::string powersave_governor; ///< empty means "not set"
    std::set<std::string> mitigation_items;
};

/**
 * @brief Migrate legacy configuration into V2 semantics. Pure: no I/O, no shell, no device.
 *
 * Deterministic by construction — same input, same output, same note order — so a migration can
 * be diffed and tested rather than observed.
 */
[[nodiscard]] RuntimeTuning migrate_legacy_config(const LegacyConfigInput &input);

/**
 * @brief Apply @p tuning to @p packs: governor choices in, suppressed capabilities out.
 *
 * Returns the packs the runtime should plan against. Suppression is expressed by *removing the
 * descriptor*, so a suppressed capability is not a candidate at all — rather than a candidate
 * that something downstream is trusted to skip.
 */
[[nodiscard]] std::vector<DevicePack> apply_tuning(std::vector<DevicePack> packs,
                                                   const RuntimeTuning &tuning);

} // namespace flux::execution
