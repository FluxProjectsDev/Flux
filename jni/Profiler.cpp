/*
 * Copyright (C) 2024-2026 Rem01Gaming
 * Copyright (C) 2024-2026 FebriCahyaa
 *
 * Adapted from Encore Tweaks (https://github.com/Rem01Gaming/encore).
 * Modified by the Flux project.
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

#include <algorithm>
#include <cstdlib>

#include "Flux.hpp"
#include "FluxLog.hpp"
#include "Profiler.hpp"
#include "Write2File.hpp"

#include "DeviceMitigationStore.hpp"
#include "FluxConfigStore.hpp"

#include <FluxUtility.hpp>
#include <utility>

namespace {
/// The single injected view onto the live telemetry authority. Empty until the daemon installs
/// it, in which case every read reports "nothing published" and the safe defaults apply.
TelemetryProvider g_telemetry_provider;
} // namespace

void set_telemetry_provider(TelemetryProvider provider) {
    g_telemetry_provider = std::move(provider);
}

namespace {
/// Read the live snapshot, or nullopt when no provider is installed / nothing is published.
std::optional<flux::telemetry::RawSnapshot> current_telemetry() {
    return g_telemetry_provider ? g_telemetry_provider() : std::nullopt;
}
} // namespace

void set_profiler_env_vars() {
    // Get preferences from config store
    auto prefs = config_store.get_preferences();

    // Clear all existing _FLUX_* environment variables
    extern char **environ;
    for (char **env = environ; *env; ++env) {
        std::string env_str(*env);
        if (env_str.find("FLUX_") == 0) {
            // Extract the variable name (up to '=')
            size_t eq_pos = env_str.find('=');
            if (eq_pos != std::string::npos) {
                std::string var_name = env_str.substr(0, eq_pos);
                unsetenv(var_name.c_str());
            }
        }
    }

    // Use cached mitigation items instead of re-evaluating rules
    auto mitigation_items = device_mitigation_store.get_cached_mitigation_items(prefs.use_device_mitigation);

    // Set environment variable for mitigation items
    for (const auto &item : mitigation_items) {
        std::string env_var = "FLUX_" + item;
        std::transform(env_var.begin(), env_var.end(), env_var.begin(), [](unsigned char c) {
            if (!std::isalnum(c) && c != '_') return '_';
            return static_cast<char>(std::toupper(c));
        });

        setenv(env_var.c_str(), "1", 1);
        LOGD_TAG("Profiler", "Set mitigation env var: {}", env_var);
    }

    // Set CPU Governor variables
    FluxConfigStore::CPUGovernor cpu_governor_preference = config_store.get_cpu_governor();
    setenv("FLUX_BALANCED_CPUGOV", cpu_governor_preference.balance.c_str(), 1);
    setenv("FLUX_POWERSAVE_CPUGOV", cpu_governor_preference.powersave.c_str(), 1);

    // Expose kernel and thermal API capabilities so the profiler shell script
    // can skip sysfs writes that are only valid on GKI kernels or API 31+.
    // Writing to unsupported sysfs nodes on certain vendor kernels can cause
    // hard hangs or kernel oops, leading to device freeze or reboot.
    {
        const auto status = current_telemetry();
        if (status.has_value()) {
            setenv("FLUX_IS_GKI_KERNEL", status->kernel_is_gki ? "1" : "0", 1);
            setenv("FLUX_THERMAL_API_AVAILABLE", status->thermal_available ? "1" : "0", 1);
        } else {
            // No telemetry yet — default to the safest / most-compatible values.
            setenv("FLUX_IS_GKI_KERNEL", "0", 1);
            setenv("FLUX_THERMAL_API_AVAILABLE", "0", 1);
        }
    }
}

void run_perfcommon(void) {
    if (config_store.get_preferences().disable_tweaks) {
        LOGI_TAG("Profiler", "Tweaks are disabled in config, skipping perfcommon");
        return;
    }

    set_profiler_env_vars();

    if (system("flux_profiler perfcommon")) {
        LOGE("Unable to execute profiler changes to perfcommon");
    }
}

void apply_performance_profile(bool lite_mode, std::string game_pkg, pid_t game_pid) {
    if (config_store.get_preferences().disable_tweaks) {
        LOGI_TAG("Profiler", "Tweaks are disabled in config, skipping performance profile");
        return;
    }

    set_profiler_env_vars();

    uid_t game_uid = 0;
    const auto status = current_telemetry();
    if (status.has_value() && status->focused_package == game_pkg && status->focused_uid > 0) {
        game_uid = static_cast<uid_t>(status->focused_uid);
    } else {
        game_uid = get_uid_by_package_name(game_pkg);
    }

    write2file(GAME_INFO, game_pkg, " ", game_pid, " ", game_uid, "\n");
    write2file(PROFILE_MODE, static_cast<int>(PERFORMANCE_PROFILE), "\n");

    if (lite_mode) {
        LOGD("Lite mode is enabled");
        if (system("flux_profiler performance_lite") != 0) {
            LOGE("Unable to execute profiler changes to performance_lite");
        }

        return;
    }

    if (system("flux_profiler performance") != 0) {
        LOGE("Unable to execute profiler changes to performance");
    }
}

void apply_performance_lite_profile(std::string game_pkg, pid_t game_pid) {
    if (config_store.get_preferences().disable_tweaks) {
        LOGI_TAG("Profiler", "Tweaks are disabled in config, skipping performance_lite profile");
        return;
    }

    set_profiler_env_vars();

    uid_t game_uid = 0;
    const auto status = current_telemetry();
    if (status.has_value() && status->focused_package == game_pkg && status->focused_uid > 0) {
        game_uid = static_cast<uid_t>(status->focused_uid);
    } else {
        game_uid = get_uid_by_package_name(game_pkg);
    }

    write2file(GAME_INFO, game_pkg, " ", game_pid, " ", game_uid, "\n");
    write2file(PROFILE_MODE, static_cast<int>(PERFORMANCE_LITE_PROFILE), "\n");

    LOGD("Thermal headroom low — applying performance_lite profile for {}", game_pkg);
    if (system("flux_profiler performance_lite") != 0) {
        LOGE("Unable to execute profiler changes to performance_lite");
    }
}

void apply_balance_profile() {
    if (config_store.get_preferences().disable_tweaks) {
        LOGI_TAG("Profiler", "Tweaks are disabled in config, skipping balance profile");
        return;
    }

    set_profiler_env_vars();

    write2file(GAME_INFO, "NULL 0 0\n");
    write2file(PROFILE_MODE, static_cast<int>(BALANCE_PROFILE), "\n");

    if (system("flux_profiler balance") != 0) {
        LOGE("Unable to execute profiler changes to balance");
    }
}

void apply_powersave_profile() {
    if (config_store.get_preferences().disable_tweaks) {
        LOGI_TAG("Profiler", "Tweaks are disabled in config, skipping powersave profile");
        return;
    }

    set_profiler_env_vars();

    write2file(GAME_INFO, "NULL 0 0\n");
    write2file(PROFILE_MODE, static_cast<int>(POWERSAVE_PROFILE), "\n");

    if (system("flux_profiler powersave") != 0) {
        LOGE("Unable to execute profiler changes to powersave");
    }
}
