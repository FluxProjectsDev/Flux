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

#include "TestFramework.hpp"

#include <SynthesisCore.hpp>

#include <string>

namespace {

/// A well-formed schema-v2 snapshot. Individual tests override single fields.
std::string valid_snapshot(
    const std::string &overrides_key = "", const std::string &overrides_value = ""
) {
    struct Field {
        const char *key;
        std::string value;
    };

    std::vector<Field> fields = {
        {"schema_version", "2"},
        {"sequence", "125"},
        {"updated_elapsed_ms", "81723451"},
        {"daemon_pid", "2841"},
        {"foreground_available", "1"},
        {"focused_package", "com.example.game"},
        {"focused_pid", "9821"},
        {"focused_uid", "10384"},
        {"screen_available", "1"},
        {"screen_awake", "1"},
        {"power_available", "1"},
        {"battery_saver", "0"},
        {"charging_available", "1"},
        {"charging_state", "1"},
        {"thermal_available", "1"},
        {"thermal_valid", "1"},
        {"thermal_headroom", "0.84"},
        {"thermal_status", "3"},
        {"thermal_sample_elapsed_ms", "81723000"},
        {"thermal_age_ms", "451"},
        {"audio_available", "1"},
        {"audio_active", "1"},
        {"zen_available", "1"},
        {"zen_mode", "0"},
        {"kernel_is_gki", "1"},
    };

    std::string out;
    for (auto &field : fields) {
        std::string value = field.value;
        if (!overrides_key.empty() && overrides_key == field.key) {
            value = overrides_value;
        }
        out += std::string(field.key) + " " + value + "\n";
    }
    return out;
}

/// Same, but with a named key removed entirely.
std::string snapshot_without(const std::string &drop_key) {
    std::string out;
    const std::string full = valid_snapshot();
    size_t pos             = 0;
    while (pos < full.size()) {
        const size_t eol  = full.find('\n', pos);
        const std::string line = full.substr(pos, eol - pos);
        if (line.rfind(drop_key + " ", 0) != 0) {
            out += line + "\n";
        }
        pos = eol + 1;
    }
    return out;
}

} // namespace

TEST("parser: accepts a well-formed v2 snapshot") {
    TelemetrySnapshot snap;
    const auto result = SynthesisCoreReader::parse(snap, valid_snapshot(), 5000);

    CHECK_MSG(result.ok(), result.detail);
    CHECK_EQ(snap.schema_version, 2);
    CHECK_EQ(snap.sequence, 125ULL);
    CHECK_EQ(snap.daemon_pid, 2841);
    CHECK_EQ(snap.focused_package, std::string("com.example.game"));
    CHECK_EQ(snap.focused_pid, 9821);
    CHECK_EQ(snap.focused_uid, 10384u);
    CHECK(snap.screen_awake);
    CHECK(snap.charging);
    CHECK(snap.has_thermal());
    CHECK_NEAR(snap.thermal_headroom, 0.84f, 1e-6);
    CHECK_EQ(snap.thermal_status, 3);
    CHECK_EQ(snap.received_monotonic_ms, 5000LL);
}

TEST("parser: rejects an unsupported schema version rather than reading it anyway") {
    TelemetrySnapshot snap;
    const auto result = SynthesisCoreReader::parse(snap, valid_snapshot("schema_version", "3"), 0);

    CHECK(!result.ok());
    CHECK_EQ(result.error, ParseError::UnsupportedSchemaVersion);
}

TEST("parser: rejects a v1 snapshot, which has no schema_version at all") {
    // Exactly what the previous producer emitted.
    const std::string v1 =
        "focused_app com.example.game 900 10100\n"
        "screen_awake 1\n"
        "battery_saver 0\n"
        "zen_mode 0\n"
        "charging_state 1\n"
        "thermal_status 0.84\n"
        "audio_active 1\n";

    TelemetrySnapshot snap;
    const auto result = SynthesisCoreReader::parse(snap, v1, 0);

    CHECK(!result.ok());
    CHECK_EQ(result.error, ParseError::MissingSchemaVersion);
}

/**
 * The locale bug, caught at the consumer.
 *
 * A producer running under an Indonesian/German/French locale would emit "0,84". strtof
 * stops at the comma and yields 0.0 — which under the corrected semantics reads as "no
 * thermal pressure whatsoever" and would suppress every downgrade. It must be an error.
 */
TEST("parser: rejects a comma-decimal float instead of silently reading zero") {
    TelemetrySnapshot snap;
    const auto result = SynthesisCoreReader::parse(snap, valid_snapshot("thermal_headroom", "0,84"), 0);

    CHECK(!result.ok());
    CHECK_EQ(result.error, ParseError::MalformedNumber);
}

TEST("parser: rejects trailing junk on a number") {
    TelemetrySnapshot snap;
    CHECK(!SynthesisCoreReader::parse(snap, valid_snapshot("focused_pid", "123abc"), 0).ok());
    CHECK(!SynthesisCoreReader::parse(snap, valid_snapshot("sequence", "12 34"), 0).ok());
}

TEST("parser: preserves thermal headroom above 1.0") {
    TelemetrySnapshot snap;
    const auto result = SynthesisCoreReader::parse(snap, valid_snapshot("thermal_headroom", "1.73"), 0);

    CHECK(result.ok());
    CHECK_NEAR(snap.thermal_headroom, 1.73f, 1e-6);
    CHECK(snap.has_thermal());
}

TEST("parser: nan headroom parses but is not a valid reading") {
    TelemetrySnapshot snap;
    std::string text = valid_snapshot("thermal_headroom", "nan");
    // The producer always pairs nan with thermal_valid 0.
    text = text.replace(text.find("thermal_valid 1"), std::string("thermal_valid 1").size(), "thermal_valid 0");

    const auto result = SynthesisCoreReader::parse(snap, text, 0);
    CHECK_MSG(result.ok(), result.detail);
    CHECK(snap.thermal_available);
    CHECK(!snap.thermal_valid);
    CHECK_MSG(!snap.has_thermal(), "nan must never count as a usable reading");
}

TEST("parser: rejects thermal_valid=1 paired with a NaN headroom") {
    TelemetrySnapshot snap;
    const auto result = SynthesisCoreReader::parse(snap, valid_snapshot("thermal_headroom", "nan"), 0);
    CHECK(!result.ok());
    CHECK_EQ(result.error, ParseError::MalformedNumber);
}

TEST("parser: rejects implausible pid and uid") {
    TelemetrySnapshot snap;
    CHECK_EQ(
        SynthesisCoreReader::parse(snap, valid_snapshot("focused_pid", "-5"), 0).error,
        ParseError::ImplausibleIdentifier
    );
    CHECK_EQ(
        SynthesisCoreReader::parse(snap, valid_snapshot("focused_pid", "99999999"), 0).error,
        ParseError::ImplausibleIdentifier
    );
    CHECK_EQ(
        SynthesisCoreReader::parse(snap, valid_snapshot("daemon_pid", "0"), 0).error,
        ParseError::ImplausibleIdentifier
    );
}

TEST("parser: a pid of zero for the focused app is legal") {
    // The producer legitimately reports 0 for one cycle while a freshly launched app is not
    // yet visible in getRunningAppProcesses().
    TelemetrySnapshot snap;
    const auto result = SynthesisCoreReader::parse(snap, valid_snapshot("focused_pid", "0"), 0);
    CHECK_MSG(result.ok(), result.detail);
    CHECK_EQ(snap.focused_pid, 0);
}

TEST("parser: detects a missing required field") {
    TelemetrySnapshot snap;
    const auto result = SynthesisCoreReader::parse(snap, snapshot_without("thermal_valid"), 0);

    CHECK(!result.ok());
    CHECK_EQ(result.error, ParseError::MissingRequiredField);
    CHECK_EQ(result.detail, std::string("thermal_valid"));
}

TEST("parser: ignores unknown forward-compatible fields") {
    TelemetrySnapshot snap;
    const std::string text = valid_snapshot() + "some_future_field 42\nanother_one hello world\n";

    const auto result = SynthesisCoreReader::parse(snap, text, 0);
    CHECK_MSG(result.ok(), result.detail);
    CHECK_EQ(snap.sequence, 125ULL);
}

TEST("parser: preserves the full zen enum, not a boolean") {
    for (int mode = 0; mode <= 3; ++mode) {
        TelemetrySnapshot snap;
        const auto result =
            SynthesisCoreReader::parse(snap, valid_snapshot("zen_mode", std::to_string(mode)), 0);
        CHECK(result.ok());
        CHECK_EQ(snap.zen_mode, mode);
    }
}

/**
 * The all-or-nothing guarantee. The previous parser wrote each field into the live cache as
 * it went, so a snapshot that went bad halfway through left the daemon acting on a mixture
 * of new and stale values.
 */
TEST("parser: a failed parse leaves the destination untouched") {
    TelemetrySnapshot snap;
    CHECK(SynthesisCoreReader::parse(snap, valid_snapshot(), 1000).ok());

    const auto before = snap;

    const auto result = SynthesisCoreReader::parse(snap, valid_snapshot("thermal_headroom", "0,84"), 2000);
    CHECK(!result.ok());

    CHECK_EQ(snap.sequence, before.sequence);
    CHECK_NEAR(snap.thermal_headroom, before.thermal_headroom, 1e-9);
    CHECK_EQ(snap.received_monotonic_ms, before.received_monotonic_ms);
}

TEST("parser: tolerates CRLF line endings and blank lines") {
    std::string text;
    const std::string full = valid_snapshot();
    size_t pos             = 0;
    while (pos < full.size()) {
        const size_t eol = full.find('\n', pos);
        text += full.substr(pos, eol - pos) + "\r\n";
        pos = eol + 1;
    }
    text += "\n\n";

    TelemetrySnapshot snap;
    const auto result = SynthesisCoreReader::parse(snap, text, 0);
    CHECK_MSG(result.ok(), result.detail);
    CHECK_EQ(snap.focused_package, std::string("com.example.game"));
}

TEST("parser: an empty or truncated file is rejected, not half-read") {
    TelemetrySnapshot snap;
    CHECK(!SynthesisCoreReader::parse(snap, "", 0).ok());
    CHECK(!SynthesisCoreReader::parse(snap, "schema_version 2\nsequence 5\n", 0).ok());
}

// --- Freshness / health ------------------------------------------------------

TEST("cache: reports Offline before any snapshot has ever been accepted") {
    SynthesisCoreCache cache;
    const FreshnessPolicy policy;

    CHECK_EQ(cache.health(0, policy), TelemetryHealth::Offline);
    CHECK_EQ(cache.age_ms(0), -1LL);
    CHECK(!cache.get().has_value());
}

TEST("cache: health degrades Healthy -> Stale -> Offline with local receipt age") {
    SynthesisCoreCache cache;
    const FreshnessPolicy policy; // stale 5s, offline 15s

    TelemetrySnapshot snap;
    CHECK(SynthesisCoreReader::parse(snap, valid_snapshot(), 10'000).ok());
    cache.update(snap);

    CHECK_EQ(cache.health(10'000, policy), TelemetryHealth::Healthy);
    CHECK_EQ(cache.health(14'999, policy), TelemetryHealth::Healthy);
    CHECK_EQ(cache.health(15'000, policy), TelemetryHealth::Stale);
    CHECK_EQ(cache.health(24'999, policy), TelemetryHealth::Stale);
    CHECK_EQ(cache.health(25'000, policy), TelemetryHealth::Offline);
}

TEST("cache: a failed parse never disturbs the last good snapshot") {
    SynthesisCoreCache cache;

    TelemetrySnapshot snap;
    CHECK(SynthesisCoreReader::parse(snap, valid_snapshot(), 1000).ok());
    cache.update(snap);

    cache.record_parse_error(ParseError::MalformedNumber, "thermal_headroom='0,84'");

    const auto held = cache.get();
    CHECK(held.has_value());
    CHECK_EQ(held->sequence, 125ULL);
    CHECK_EQ(cache.parse_error_count(), 1ULL);
    CHECK_EQ(cache.last_error(), ParseError::MalformedNumber);
}
