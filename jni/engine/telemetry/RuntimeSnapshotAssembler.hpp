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

#include <optional>
#include <string>

#include <DecisionEngine.hpp> // flux::engine::RuntimeSnapshot / CapabilitySnapshot / DataHealth

#include "TelemetrySnapshot.hpp"

namespace flux::telemetry {

/**
 * @brief Which provider supplied the foreground/process information.
 *
 * The Decision Engine never learns which of these produced the foreground data — that is the
 * point of the assembler. The ordering encodes the long-term priority: an independent native
 * ProcessEventSource first (a future phase — no Encore binder code is used), the SynthesisCore
 * foreground fallback second, degraded-unavailable last.
 */
enum class ForegroundProvider {
    NativeProcessEventSource,
    SynthesisCoreFallback,
    Unavailable,
};

const char *foreground_provider_name(ForegroundProvider provider);

/** Provider-neutral foreground/process information. */
struct ForegroundInfo {
    bool present = false;
    std::string package = kPackageNone;
    int pid = 0;
    int uid = 0;
};

struct ForegroundResolution {
    ForegroundProvider source = ForegroundProvider::Unavailable;
    ForegroundInfo info;
};

/** Everything the Decision Engine consumes, assembled from one telemetry state. */
struct AssembledRuntime {
    flux::engine::RuntimeSnapshot runtime;
    flux::engine::CapabilitySnapshot capabilities;
    ForegroundResolution foreground;
};

/**
 * @brief Normalizes raw telemetry + health + a foreground resolution into the Decision
 *        Engine's provider-neutral RuntimeSnapshot.
 */
class RuntimeSnapshotAssembler {
public:
    [[nodiscard]] AssembledRuntime assemble(const RawSnapshot &raw, TelemetryHealth health,
                                            const ForegroundResolution &foreground) const;

    /** Map telemetry health onto the Decision Engine's three-state DataHealth. */
    static flux::engine::DataHealth to_data_health(TelemetryHealth health);

    /** Derive a SynthesisCore-fallback foreground resolution from a raw snapshot. */
    static ForegroundResolution foreground_from_synthesiscore(const RawSnapshot &raw);

    /**
     * @brief Provider-neutral foreground selection.
     *
     * Prefers a present native ProcessEventSource resolution; otherwise falls back to the
     * SynthesisCore foreground; otherwise reports Unavailable. @p native is std::nullopt
     * until the native source exists.
     */
    static ForegroundResolution select_foreground(const std::optional<ForegroundResolution> &native,
                                                  const RawSnapshot &raw);
};

} // namespace flux::telemetry
