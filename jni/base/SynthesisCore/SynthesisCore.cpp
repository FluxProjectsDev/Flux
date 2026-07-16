/*
 * Copyright (C) 2024-2026 FebriCahyaa
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

#include <SynthesisCore.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

// Global singleton
SynthesisCoreCache synthesis_core_cache;

int64_t flux_monotonic_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

const char *parse_error_string(ParseError err) {
    switch (err) {
        case ParseError::None: return "none";
        case ParseError::FileUnreadable: return "file_unreadable";
        case ParseError::MissingSchemaVersion: return "missing_schema_version";
        case ParseError::UnsupportedSchemaVersion: return "unsupported_schema_version";
        case ParseError::MalformedNumber: return "malformed_number";
        case ParseError::ImplausibleIdentifier: return "implausible_identifier";
        case ParseError::MissingRequiredField: return "missing_required_field";
    }
    return "unknown";
}

const char *telemetry_health_string(TelemetryHealth health) {
    switch (health) {
        case TelemetryHealth::Healthy: return "healthy";
        case TelemetryHealth::Stale: return "stale";
        case TelemetryHealth::Offline: return "offline";
    }
    return "unknown";
}

namespace {

/// Linux PIDs are bounded by /proc/sys/kernel/pid_max, which cannot exceed 2^22.
/// Anything beyond that is not a PID we could have been handed.
constexpr long PID_MAX_PLAUSIBLE = 4 * 1024 * 1024;

/// Android app UIDs run to 19999 per user, and multi-user offsets each user by 100000.
/// A few million is generous; a billion is a parse error.
constexpr long UID_MAX_PLAUSIBLE = 100 * 1000 * 1000;

/**
 * @brief Strict integer parse. The token must be consumed in its entirety.
 *
 * strtoll stopping early is the whole failure mode we are guarding against: it is what
 * turns "0,84" into 0 and "12abc" into 12. Requiring full consumption makes a malformed
 * value an error instead of a plausible-looking wrong number.
 */
bool parse_int64(const std::string &token, int64_t &out) {
    if (token.empty()) return false;

    errno         = 0;
    char *end     = nullptr;
    const long long v = std::strtoll(token.c_str(), &end, 10);

    if (errno == ERANGE) return false;
    if (end != token.c_str() + token.size()) return false; // trailing junk

    out = static_cast<int64_t>(v);
    return true;
}

/**
 * @brief Strict float parse, rejecting locale-corrupted values.
 *
 * strtof honours LC_NUMERIC. Flux never calls setlocale, so it runs in the "C" locale and
 * expects a '.' separator — but rather than rely on that invariant holding forever, a token
 * that is not fully consumed is rejected. That is exactly what catches the "0,84" a
 * comma-decimal producer would emit: strtof stops at the comma, leaves trailing junk, and
 * we reject instead of silently reading 0.0 — which under the corrected thermal semantics
 * would mean "perfectly cool" and would suppress every downgrade.
 *
 * The literal "nan" is accepted, because the producer emits it deliberately alongside
 * thermal_valid=0.
 */
bool parse_float(const std::string &token, float &out) {
    if (token.empty()) return false;

    if (token == "nan" || token == "NaN") {
        out = std::numeric_limits<float>::quiet_NaN();
        return true;
    }

    errno     = 0;
    char *end = nullptr;
    const float v = std::strtof(token.c_str(), &end);

    if (errno == ERANGE) return false;
    if (end != token.c_str() + token.size()) return false; // trailing junk, e.g. "0,84"

    out = v;
    return true;
}

bool parse_bool(const std::string &token, bool &out) {
    int64_t v = 0;
    if (!parse_int64(token, v)) return false;
    out = (v != 0);
    return true;
}

} // namespace

namespace SynthesisCoreReader {

ParseResult parse(TelemetrySnapshot &out, const std::string &text, int64_t received_monotonic_ms) {
    // Assemble into a temporary. `out` is not touched until the whole snapshot validates,
    // so a rejected update can never leave the caller holding half-new, half-stale fields.
    TelemetrySnapshot snap;
    snap.received_monotonic_ms = received_monotonic_ms;

    std::unordered_map<std::string, std::string> fields;
    fields.reserve(32);

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back(); // tolerate CRLF
        if (line.empty()) continue;

        const size_t sep = line.find(' ');
        if (sep == std::string::npos || sep == 0) continue; // not a key/value line; ignore

        fields.emplace(line.substr(0, sep), line.substr(sep + 1));
    }

    // --- Schema gate --------------------------------------------------------
    // Checked before anything else: reading a foreign schema on a best-effort basis is
    // how field semantics silently drift.
    const auto schema_it = fields.find("schema_version");
    if (schema_it == fields.end()) {
        return {ParseError::MissingSchemaVersion, "no schema_version key"};
    }

    int64_t schema = 0;
    if (!parse_int64(schema_it->second, schema)) {
        return {ParseError::MalformedNumber, "schema_version='" + schema_it->second + "'"};
    }
    if (schema != SYNTHESIS_SCHEMA_VERSION) {
        return {
            ParseError::UnsupportedSchemaVersion,
            "got schema " + std::to_string(schema) + ", this build speaks " +
                std::to_string(SYNTHESIS_SCHEMA_VERSION)
        };
    }
    snap.schema_version = static_cast<int>(schema);

    // --- Helpers ------------------------------------------------------------
    ParseResult failure;

    auto require_int = [&](const char *key, int64_t &dst) -> bool {
        const auto it = fields.find(key);
        if (it == fields.end()) {
            failure = {ParseError::MissingRequiredField, key};
            return false;
        }
        if (!parse_int64(it->second, dst)) {
            failure = {ParseError::MalformedNumber, std::string(key) + "='" + it->second + "'"};
            return false;
        }
        return true;
    };

    auto require_bool = [&](const char *key, bool &dst) -> bool {
        const auto it = fields.find(key);
        if (it == fields.end()) {
            failure = {ParseError::MissingRequiredField, key};
            return false;
        }
        if (!parse_bool(it->second, dst)) {
            failure = {ParseError::MalformedNumber, std::string(key) + "='" + it->second + "'"};
            return false;
        }
        return true;
    };

    // --- Envelope -----------------------------------------------------------
    int64_t sequence = 0, updated = 0, daemon_pid = 0;
    if (!require_int("sequence", sequence)) return failure;
    if (!require_int("updated_elapsed_ms", updated)) return failure;
    if (!require_int("daemon_pid", daemon_pid)) return failure;

    if (sequence < 0 || updated < 0) {
        return {ParseError::MalformedNumber, "negative sequence or timestamp"};
    }
    if (daemon_pid <= 0 || daemon_pid > PID_MAX_PLAUSIBLE) {
        return {ParseError::ImplausibleIdentifier, "daemon_pid=" + std::to_string(daemon_pid)};
    }

    snap.sequence           = static_cast<uint64_t>(sequence);
    snap.updated_elapsed_ms = updated;
    snap.daemon_pid         = static_cast<pid_t>(daemon_pid);

    // --- Foreground ---------------------------------------------------------
    if (!require_bool("foreground_available", snap.foreground_available)) return failure;

    const auto pkg_it = fields.find("focused_package");
    if (pkg_it == fields.end()) {
        return {ParseError::MissingRequiredField, "focused_package"};
    }
    snap.focused_package = pkg_it->second;

    int64_t fpid = 0, fuid = 0;
    if (!require_int("focused_pid", fpid)) return failure;
    if (!require_int("focused_uid", fuid)) return failure;

    // A PID of 0 is legitimate: the producer reports it for one cycle while a freshly
    // launched app is not yet in getRunningAppProcesses(). Negative or absurd is not.
    if (fpid < 0 || fpid > PID_MAX_PLAUSIBLE) {
        return {ParseError::ImplausibleIdentifier, "focused_pid=" + std::to_string(fpid)};
    }
    if (fuid < 0 || fuid > UID_MAX_PLAUSIBLE) {
        return {ParseError::ImplausibleIdentifier, "focused_uid=" + std::to_string(fuid)};
    }
    snap.focused_pid = static_cast<pid_t>(fpid);
    snap.focused_uid = static_cast<uid_t>(fuid);

    // --- Screen / power / charging ------------------------------------------
    if (!require_bool("screen_available", snap.screen_available)) return failure;
    if (!require_bool("screen_awake", snap.screen_awake)) return failure;
    if (!require_bool("power_available", snap.power_available)) return failure;
    if (!require_bool("battery_saver", snap.battery_saver)) return failure;
    if (!require_bool("charging_available", snap.charging_available)) return failure;
    if (!require_bool("charging_state", snap.charging)) return failure;

    // --- Thermal ------------------------------------------------------------
    if (!require_bool("thermal_available", snap.thermal_available)) return failure;
    if (!require_bool("thermal_valid", snap.thermal_valid)) return failure;

    const auto headroom_it = fields.find("thermal_headroom");
    if (headroom_it == fields.end()) {
        return {ParseError::MissingRequiredField, "thermal_headroom"};
    }
    if (!parse_float(headroom_it->second, snap.thermal_headroom)) {
        return {ParseError::MalformedNumber, "thermal_headroom='" + headroom_it->second + "'"};
    }

    // Trust the producer's own validity flag over the number it shipped. A producer that
    // says "not valid" but happens to emit a parseable float must not be treated as valid.
    if (snap.thermal_valid && std::isnan(snap.thermal_headroom)) {
        return {ParseError::MalformedNumber, "thermal_valid=1 but headroom is NaN"};
    }

    int64_t tstatus = 0, tsample = 0, tage = 0;
    if (!require_int("thermal_status", tstatus)) return failure;
    if (!require_int("thermal_sample_elapsed_ms", tsample)) return failure;
    if (!require_int("thermal_age_ms", tage)) return failure;

    snap.thermal_status            = static_cast<int>(tstatus);
    snap.thermal_sample_elapsed_ms = tsample;
    snap.thermal_age_ms            = tage;

    // --- Audio / zen / kernel -----------------------------------------------
    if (!require_bool("audio_available", snap.audio_available)) return failure;
    if (!require_bool("audio_active", snap.audio_active)) return failure;
    if (!require_bool("zen_available", snap.zen_available)) return failure;

    int64_t zen = 0;
    if (!require_int("zen_mode", zen)) return failure;
    // Preserve the full enum. Collapsing anything non-zero to a boolean is what made Flux
    // restore "priority" as plain "on", silently changing the user's setting.
    snap.zen_mode = static_cast<int>(zen);

    if (!require_bool("kernel_is_gki", snap.kernel_is_gki)) return failure;

    // Fully validated: publish as one whole value.
    out = std::move(snap);
    return {};
}

ParseResult read(TelemetrySnapshot &out, const char *path, int64_t received_monotonic_ms) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) {
        return {ParseError::FileUnreadable, path};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad()) {
        return {ParseError::FileUnreadable, path};
    }

    return parse(out, buffer.str(), received_monotonic_ms);
}

} // namespace SynthesisCoreReader
