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

#include "RuntimeSnapshotAssembler.hpp"

namespace flux::telemetry {

const char *foreground_provider_name(ForegroundProvider provider) {
    switch (provider) {
        case ForegroundProvider::NativeProcessEventSource: return "native_process_event_source";
        case ForegroundProvider::SynthesisCoreFallback: return "synthesiscore_fallback";
        case ForegroundProvider::Unavailable: return "unavailable";
    }
    return "unknown";
}

flux::engine::DataHealth RuntimeSnapshotAssembler::to_data_health(TelemetryHealth health) {
    switch (health) {
        case TelemetryHealth::Healthy:
            return flux::engine::DataHealth::Healthy;
        case TelemetryHealth::Delayed:
        case TelemetryHealth::Stale:
            // Both mean "hold current state, never promote" to the engine.
            return flux::engine::DataHealth::Stale;
        case TelemetryHealth::Invalid:
        case TelemetryHealth::Unavailable:
        case TelemetryHealth::UnsupportedSchema:
            return flux::engine::DataHealth::Offline;
    }
    return flux::engine::DataHealth::Offline;
}

ForegroundResolution RuntimeSnapshotAssembler::foreground_from_synthesiscore(const RawSnapshot &raw) {
    ForegroundResolution r;
    if (!raw.foreground_available || raw.focused_package == kPackageNone ||
        raw.focused_package.empty()) {
        r.source = ForegroundProvider::Unavailable;
        return r;
    }
    r.source = ForegroundProvider::SynthesisCoreFallback;
    r.info.present = true;
    r.info.package = raw.focused_package;
    r.info.pid = raw.focused_pid;
    r.info.uid = raw.focused_uid;
    return r;
}

ForegroundResolution
RuntimeSnapshotAssembler::select_foreground(const std::optional<ForegroundResolution> &native,
                                            const RawSnapshot &raw) {
    if (native && native->source == ForegroundProvider::NativeProcessEventSource &&
        native->info.present) {
        return *native;
    }
    return foreground_from_synthesiscore(raw);
}

AssembledRuntime RuntimeSnapshotAssembler::assemble(const RawSnapshot &raw, TelemetryHealth health,
                                                    const ForegroundResolution &foreground) const {
    AssembledRuntime out;
    out.foreground = foreground;

    out.capabilities.thermal_supported = raw.thermal_available;

    flux::engine::RuntimeSnapshot &rt = out.runtime;
    rt.health = to_data_health(health);

    if (raw.has_thermal()) {
        rt.thermal = flux::engine::ThermalReading{raw.thermal_headroom, raw.thermal_status};
    }

    // Unavailable providers degrade to the safe interpretation, never a false reading.
    rt.screen_awake = raw.screen_available ? raw.screen_awake : true;
    rt.battery_saver = raw.power_available && raw.battery_saver;
    rt.charging = raw.charging_available && raw.charging;
    rt.audio_active = raw.audio_available && raw.audio_active;
    return out;
}

} // namespace flux::telemetry
