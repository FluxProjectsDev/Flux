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

#include <functional>
#include <optional>

#include <TelemetrySnapshot.hpp> // flux::telemetry::RawSnapshot

/**
 * @brief Supplies the profiler with the current telemetry snapshot.
 *
 * The profiler must not own or reach into a telemetry cache of its own. There is exactly one
 * live telemetry authority — TelemetryRuntime, owned by the daemon — and the profiler reads it
 * through this injected provider. That keeps the ownership explicit, prevents a second telemetry
 * path from reappearing, and lets host tests drive the profiler without a device.
 *
 * Returning nullopt means "nothing valid has been published"; the profiler must then choose the
 * safest defaults rather than treating absent capabilities as present.
 */
using TelemetryProvider = std::function<std::optional<flux::telemetry::RawSnapshot>()>;

/** Install the provider. Called once by the daemon at startup. */
void set_telemetry_provider(TelemetryProvider provider);

/**
 * @brief Sets all environment variables for the profiler
 */
void set_profiler_env_vars();

void run_perfcommon(void);
void apply_performance_profile(bool lite_mode, std::string game_pkg, pid_t game_pid);
void apply_performance_lite_profile(std::string game_pkg, pid_t game_pid);
void apply_balance_profile();
void apply_powersave_profile();
