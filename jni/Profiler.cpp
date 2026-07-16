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


#include "Profiler.hpp"


#include <utility>

namespace {
/// The single injected view onto the live telemetry authority. Empty until the daemon installs
/// it, in which case every read reports "nothing published" and the safe defaults apply.
TelemetryProvider g_telemetry_provider;
} // namespace

void set_telemetry_provider(TelemetryProvider provider) {
    g_telemetry_provider = std::move(provider);
}

// The profile-apply dispatchers that used to live here — run_perfcommon,
// apply_performance_profile, apply_performance_lite_profile, apply_balance_profile,
// apply_powersave_profile and set_profiler_env_vars — are gone as of the V2 execution cutover.
//
// They were the daemon's only callers of system("flux_profiler ..."), and the reason a profile
// could be reported as applied when nothing had been written: the shell discarded every error,
// so a device with no such node and a device that was tuned looked identical from here.
//
// Profile application now goes through flux::execution::ExecutionRuntime, which plans against
// probed capabilities, writes through one audited backend, verifies by read-back, and rolls back
// what it cannot verify. There is no second write path and no shell fallback.
//
// What remains below is the telemetry provider seam. scripts/flux_profiler.sh is still packaged
// and is removed, with this file, in Increment 5.
