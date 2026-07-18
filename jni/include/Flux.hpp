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

#pragma once

#include <string>

#define NOTIFY_TITLE "Flux"
#define LOG_TAG "FluxTweaks"

#define CONFIG_DIR "/data/adb/.config/flux"
#define MODPATH "/data/adb/modules/flux"

#define LOCK_FILE CONFIG_DIR "/.lock"
#define JAVA_LOCK_FILE CONFIG_DIR "/java.lock"
#define LOG_FILE CONFIG_DIR "/flux.log"
#define PROFILE_MODE CONFIG_DIR "/current_profile"
#define GAME_INFO CONFIG_DIR "/gameinfo"
#define CONFIG_FILE CONFIG_DIR "/config.json"
#define DEVICE_MITIGATION_FILE CONFIG_DIR "/device_mitigation.json"
#define DEFAULT_CPU_GOV CONFIG_DIR "/default_cpu_gov"
#define FLUX_GAMELIST CONFIG_DIR "/gamelist.json"
#define SYNTHESIS_CORE_FILE CONFIG_DIR "/synthesis_core.json"

#define MODULE_PROP MODPATH "/module.prop"
#define MODULE_UPDATE MODPATH "/update"

enum FluxProfileMode : char {
    PERFCOMMON,
    PERFORMANCE_PROFILE,
    PERFORMANCE_LITE_PROFILE, ///< Thermal-throttled tier between performance and balance
    BALANCE_PROFILE,
    POWERSAVE_PROFILE
};

/// Thermal headroom threshold below which the daemon downgrades from
/// PERFORMANCE_PROFILE to PERFORMANCE_LITE_PROFILE.
/// Value in [0.0, 1.0] — 0.20 means "less than 20% headroom remaining".
static constexpr float THERMAL_LITE_THRESHOLD = 0.20f;

/// Thermal headroom at which the daemon upgrades back to PERFORMANCE_PROFILE.
/// Hysteresis gap prevents rapid oscillation between tiers.
static constexpr float THERMAL_RECOVER_THRESHOLD = 0.35f;

struct FluxGameList {
    std::string package_name;
    bool lite_mode;
    bool enable_dnd;
};
