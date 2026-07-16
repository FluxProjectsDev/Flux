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

#include <string>

#include "TelemetrySnapshot.hpp"

namespace flux::telemetry {

/** Why a decode failed. Surfaced with field context, never swallowed. */
enum class DecodeError {
    Ok,
    EmptyInput,
    InputTooLarge,
    LineTooLong,
    TooManyLines,
    DuplicateKey,
    MissingSchemaVersion,
    UnsupportedSchema,
    MissingRequiredField,
    MalformedInteger,
    MalformedFloat,
    NonFiniteValue,        ///< +/-inf, or NaN where a valid number was promised
    ImplausibleIdentifier, ///< negative pid/uid
};

const char *decode_error_name(DecodeError error);

struct DecodeResult {
    DecodeError error = DecodeError::Ok;
    std::string detail; ///< field name / offending token, for diagnostics
    RawSnapshot snapshot;

    [[nodiscard]] bool ok() const { return error == DecodeError::Ok; }
};

/**
 * @brief Strict, deterministic, side-effect-free schema-v2 decoder.
 *
 * Decoding is all-or-nothing: the snapshot is assembled in a local and only returned
 * on full success, so a caller can never observe a half-decoded mixture. Input is
 * bounded (total size, line length, line count) to prevent unbounded allocation from
 * malformed input. Unknown optional keys are ignored; duplicate keys and any malformed
 * required field are rejected with context.
 */
class TelemetryDecoder {
public:
    /// Guard rails against hostile or corrupt input.
    static constexpr size_t kMaxInputBytes = 64 * 1024;
    static constexpr size_t kMaxLineBytes = 4 * 1024;
    static constexpr size_t kMaxLines = 256;
    static constexpr size_t kMaxPackageLen = 256;

    /**
     * @brief Decode @p text into a snapshot.
     * @param received_monotonic_ms Flux's monotonic clock at receipt; copied into the result.
     */
    [[nodiscard]] DecodeResult decode(const std::string &text, int64_t received_monotonic_ms) const;
};

} // namespace flux::telemetry
