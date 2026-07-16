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

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include <Flux.hpp>
#include <FluxLog.hpp>

/**
 * @brief  Fetches PID of a process.
 * @param  target_name: The package name.
 * @param  strict: Strict matching mode. If true, "com.mobile.legends" will NOT match "com.mobile.legends:UnityKillsMe"
 * @return The PID of the package if found, otherwise 0.
 */
[[nodiscard]] pid_t pidof(std::string_view target_name, bool strict = true);

/**
 * @brief Retrieves the UID of a given package name by checking its data directory.
 *
 * @param package_name The package name of the application (e.g., "com.termux").
 * @return The UID of the package if found, otherwise 0.
 */
[[nodiscard]] uid_t get_uid_by_package_name(const std::string &package_name);

/**
 * @brief Posts a notification via shell.
 *
 * @param message The message content of the notification.
 * @note It is only intended for use in an Android environment.
 */
void notify(const char *message);

/**
 * @brief Android's Settings.Global.ZEN_MODE_* values.
 *
 * SynthesisCore reports this as an integer, and it must stay one all the way through. There
 * are four distinct modes and only the first is "off" — flattening the other three into a
 * single boolean loses which one the user had chosen.
 */
enum ZenMode : int {
    ZEN_MODE_OFF                    = 0,
    ZEN_MODE_IMPORTANT_INTERRUPTIONS = 1, ///< "Priority only"
    ZEN_MODE_NO_INTERRUPTIONS       = 2,  ///< "Total silence"
    ZEN_MODE_ALARMS                 = 3,  ///< "Alarms only"
};

/** Translate a zen mode to the argument `cmd notification set_dnd` expects. */
const char *zen_mode_to_dnd_arg(int zen_mode);

/**
 * @brief Restore an exact zen mode via shell.
 *
 * Use this — not set_do_not_disturb() — whenever restoring a mode that came from telemetry.
 * A user in "total silence" or "alarms only" must get that mode back, not "priority".
 *
 * @note It is only intended for use in an Android environment.
 */
void set_zen_mode(int zen_mode);

/**
 * @brief Turn DND on (as "priority only") or off.
 *
 * Suitable for a game *requesting* DND. Not suitable for restoring a previous state; see
 * set_zen_mode().
 *
 * @param do_not_disturb True to enable DND mode, false to disable.
 * @note It is only intended for use in an Android environment.
 */
void set_do_not_disturb(bool do_not_disturb);
