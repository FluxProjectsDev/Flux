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

#include "TelemetryDecoder.hpp"

#include <cerrno>
#include <cstdlib>
#include <string_view>
#include <unordered_map>

namespace flux::telemetry {

const char *decode_error_name(DecodeError error) {
    switch (error) {
        case DecodeError::Ok: return "ok";
        case DecodeError::EmptyInput: return "empty_input";
        case DecodeError::InputTooLarge: return "input_too_large";
        case DecodeError::LineTooLong: return "line_too_long";
        case DecodeError::TooManyLines: return "too_many_lines";
        case DecodeError::DuplicateKey: return "duplicate_key";
        case DecodeError::MissingSchemaVersion: return "missing_schema_version";
        case DecodeError::UnsupportedSchema: return "unsupported_schema";
        case DecodeError::MissingRequiredField: return "missing_required_field";
        case DecodeError::MalformedInteger: return "malformed_integer";
        case DecodeError::MalformedFloat: return "malformed_float";
        case DecodeError::NonFiniteValue: return "non_finite_value";
        case DecodeError::ImplausibleIdentifier: return "implausible_identifier";
    }
    return "unknown";
}

namespace {

/// Parse a whole string as a base-10 integer, rejecting trailing junk and overflow.
bool parse_i64(const std::string &s, int64_t &out) {
    if (s.empty()) return false;
    errno = 0;
    char *end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end != s.c_str() + s.size()) return false;
    out = static_cast<int64_t>(v);
    return true;
}

/// Parse "0" or "1" as a boolean. Anything else is malformed.
bool parse_bool(const std::string &s, bool &out) {
    if (s == "0") {
        out = false;
        return true;
    }
    if (s == "1") {
        out = true;
        return true;
    }
    return false;
}

enum class FloatKind { Finite, IsNaN, IsInf, Malformed };

/// strtof consumes "nan"/"inf" fully; we classify the outcome rather than silently
/// coercing. The decimal point is '.' in the C locale, matching the producer's Locale.ROOT.
FloatKind parse_float(const std::string &s, float &out) {
    if (s.empty()) return FloatKind::Malformed;
    errno = 0;
    char *end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return FloatKind::Malformed;
    out = v;
    if (std::isinf(v)) return FloatKind::IsInf;
    if (std::isnan(v)) return FloatKind::IsNaN;
    return FloatKind::Finite;
}

} // namespace

DecodeResult TelemetryDecoder::decode(const std::string &text, int64_t received_monotonic_ms) const {
    DecodeResult result;
    auto fail = [&](DecodeError e, std::string detail) {
        result.error = e;
        result.detail = std::move(detail);
        result.snapshot = RawSnapshot{}; // never leak partial state on failure
        return result;
    };

    if (text.empty()) return fail(DecodeError::EmptyInput, "");
    if (text.size() > kMaxInputBytes) return fail(DecodeError::InputTooLarge, std::to_string(text.size()));

    std::unordered_map<std::string, std::string> fields;
    fields.reserve(32);

    size_t line_count = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string_view line(text.data() + pos, nl - pos);
        pos = nl + 1;

        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;

        if (++line_count > kMaxLines) return fail(DecodeError::TooManyLines, std::to_string(line_count));
        if (line.size() > kMaxLineBytes) return fail(DecodeError::LineTooLong, std::to_string(line.size()));

        const size_t sp = line.find(' ');
        std::string key = std::string(sp == std::string_view::npos ? line : line.substr(0, sp));
        std::string value =
            sp == std::string_view::npos ? std::string() : std::string(line.substr(sp + 1));

        if (!fields.emplace(std::move(key), std::move(value)).second) {
            return fail(DecodeError::DuplicateKey, std::string(line.substr(0, sp)));
        }
    }

    RawSnapshot s;
    s.received_monotonic_ms = received_monotonic_ms;

    // --- schema first: reject an unreadable version before touching anything else ---
    auto sv = fields.find("schema_version");
    if (sv == fields.end()) return fail(DecodeError::MissingSchemaVersion, "");
    int64_t schema = 0;
    if (!parse_i64(sv->second, schema)) return fail(DecodeError::MalformedInteger, "schema_version");
    if (schema < kSchemaMin || schema > kSchemaMax) {
        return fail(DecodeError::UnsupportedSchema, "schema_version=" + std::to_string(schema));
    }
    s.schema_version = static_cast<int>(schema);

    // --- typed field extractors ------------------------------------------------
    auto req_i64 = [&](const char *key, int64_t &out) -> DecodeError {
        auto it = fields.find(key);
        if (it == fields.end()) {
            result.detail = key;
            return DecodeError::MissingRequiredField;
        }
        if (!parse_i64(it->second, out)) {
            result.detail = key;
            return DecodeError::MalformedInteger;
        }
        return DecodeError::Ok;
    };
    auto req_bool = [&](const char *key, bool &out) -> DecodeError {
        auto it = fields.find(key);
        if (it == fields.end()) {
            result.detail = key;
            return DecodeError::MissingRequiredField;
        }
        if (!parse_bool(it->second, out)) {
            result.detail = key;
            return DecodeError::MalformedInteger;
        }
        return DecodeError::Ok;
    };

    int64_t tmp = 0;
    DecodeError e = DecodeError::Ok;

    if ((e = req_i64("sequence", tmp)) != DecodeError::Ok) return fail(e, result.detail);
    if (tmp < 0) return fail(DecodeError::ImplausibleIdentifier, "sequence");
    s.sequence = static_cast<uint64_t>(tmp);

    if ((e = req_i64("updated_elapsed_ms", s.updated_elapsed_ms)) != DecodeError::Ok) return fail(e, result.detail);
    if ((e = req_i64("daemon_pid", tmp)) != DecodeError::Ok) return fail(e, result.detail);
    if (tmp < 0) return fail(DecodeError::ImplausibleIdentifier, "daemon_pid");
    s.daemon_pid = static_cast<int>(tmp);

    if ((e = req_bool("foreground_available", s.foreground_available)) != DecodeError::Ok) return fail(e, result.detail);

    auto pkg = fields.find("focused_package");
    if (pkg == fields.end()) return fail(DecodeError::MissingRequiredField, "focused_package");
    if (pkg->second.size() > kMaxPackageLen) return fail(DecodeError::LineTooLong, "focused_package");
    s.focused_package = pkg->second.empty() ? kPackageNone : pkg->second;

    if ((e = req_i64("focused_pid", tmp)) != DecodeError::Ok) return fail(e, result.detail);
    if (tmp < 0) return fail(DecodeError::ImplausibleIdentifier, "focused_pid=" + std::to_string(tmp));
    s.focused_pid = static_cast<int>(tmp);
    if ((e = req_i64("focused_uid", tmp)) != DecodeError::Ok) return fail(e, result.detail);
    if (tmp < 0) return fail(DecodeError::ImplausibleIdentifier, "focused_uid=" + std::to_string(tmp));
    s.focused_uid = static_cast<int>(tmp);

    if ((e = req_bool("screen_available", s.screen_available)) != DecodeError::Ok) return fail(e, result.detail);
    if ((e = req_bool("screen_awake", s.screen_awake)) != DecodeError::Ok) return fail(e, result.detail);
    if ((e = req_bool("power_available", s.power_available)) != DecodeError::Ok) return fail(e, result.detail);
    if ((e = req_bool("battery_saver", s.battery_saver)) != DecodeError::Ok) return fail(e, result.detail);
    if ((e = req_bool("charging_available", s.charging_available)) != DecodeError::Ok) return fail(e, result.detail);
    if ((e = req_bool("charging_state", s.charging)) != DecodeError::Ok) return fail(e, result.detail);

    if ((e = req_bool("thermal_available", s.thermal_available)) != DecodeError::Ok) return fail(e, result.detail);
    if ((e = req_bool("thermal_valid", s.thermal_valid)) != DecodeError::Ok) return fail(e, result.detail);

    auto hr = fields.find("thermal_headroom");
    if (hr == fields.end()) return fail(DecodeError::MissingRequiredField, "thermal_headroom");
    switch (parse_float(hr->second, s.thermal_headroom)) {
        case FloatKind::Malformed:
            return fail(DecodeError::MalformedFloat, "thermal_headroom='" + hr->second + "'");
        case FloatKind::IsInf:
            return fail(DecodeError::NonFiniteValue, "thermal_headroom is infinite");
        case FloatKind::IsNaN:
            // NaN is only legitimate alongside thermal_valid=0 (the producer's explicit
            // "no reading" marker). A NaN promised as valid is corruption.
            if (s.thermal_valid) return fail(DecodeError::NonFiniteValue, "thermal_valid=1 but headroom is NaN");
            break;
        case FloatKind::Finite:
            break; // values above 1.0 are legal and preserved
    }

    if ((e = req_i64("thermal_status", tmp)) != DecodeError::Ok) return fail(e, result.detail);
    s.thermal_status = static_cast<int>(tmp);
    if ((e = req_i64("thermal_sample_elapsed_ms", s.thermal_sample_elapsed_ms)) != DecodeError::Ok) return fail(e, result.detail);
    if ((e = req_i64("thermal_age_ms", s.thermal_age_ms)) != DecodeError::Ok) return fail(e, result.detail);

    if ((e = req_bool("audio_available", s.audio_available)) != DecodeError::Ok) return fail(e, result.detail);
    if ((e = req_bool("audio_active", s.audio_active)) != DecodeError::Ok) return fail(e, result.detail);

    if ((e = req_bool("zen_available", s.zen_available)) != DecodeError::Ok) return fail(e, result.detail);
    if ((e = req_i64("zen_mode", tmp)) != DecodeError::Ok) return fail(e, result.detail);
    s.zen_mode = static_cast<int>(tmp); // preserved as a full integer, never flattened

    if ((e = req_bool("kernel_is_gki", s.kernel_is_gki)) != DecodeError::Ok) return fail(e, result.detail);

    result.error = DecodeError::Ok;
    result.detail.clear();
    result.snapshot = s;
    return result;
}

} // namespace flux::telemetry
