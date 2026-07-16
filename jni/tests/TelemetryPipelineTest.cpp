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

// Host tests for the Flux V2 telemetry pipeline: strict decode, freshness/health,
// provider-neutral assembly, and the thread-safe store.

#include "TestFramework.hpp"

#include "RuntimeSnapshotAssembler.hpp"
#include "TelemetryDecoder.hpp"
#include "TelemetryFreshness.hpp"
#include "TelemetryStore.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace flux::telemetry;

namespace {

// Canonical, complete, valid schema-v2 wire text as an ordered key/value list, so tests can
// tweak a single field, drop one, or append an unknown key without hand-writing the whole thing.
std::vector<std::pair<std::string, std::string>> canonical() {
    return {
        {"schema_version", "2"},
        {"sequence", "42"},
        {"updated_elapsed_ms", "100000"},
        {"daemon_pid", "1234"},
        {"foreground_available", "1"},
        {"focused_package", "com.example.game"},
        {"focused_pid", "5678"},
        {"focused_uid", "10123"},
        {"screen_available", "1"},
        {"screen_awake", "1"},
        {"power_available", "1"},
        {"battery_saver", "0"},
        {"charging_available", "1"},
        {"charging_state", "0"},
        {"thermal_available", "1"},
        {"thermal_valid", "1"},
        {"thermal_headroom", "0.42"},
        {"thermal_status", "0"},
        {"thermal_sample_elapsed_ms", "99500"},
        {"thermal_age_ms", "500"},
        {"audio_available", "1"},
        {"audio_active", "0"},
        {"zen_available", "1"},
        {"zen_mode", "0"},
        {"kernel_is_gki", "1"},
    };
}

std::string to_wire(const std::vector<std::pair<std::string, std::string>> &kv) {
    std::string out;
    for (const auto &[k, v] : kv) {
        out += k;
        out += ' ';
        out += v;
        out += '\n';
    }
    return out;
}

std::string with(std::string key, std::string value) {
    auto kv = canonical();
    for (auto &p : kv) {
        if (p.first == key) {
            p.second = std::move(value);
            return to_wire(kv);
        }
    }
    kv.emplace_back(std::move(key), std::move(value)); // append as an unknown key
    return to_wire(kv);
}

std::string without(const std::string &key) {
    auto kv = canonical();
    std::vector<std::pair<std::string, std::string>> filtered;
    for (auto &p : kv)
        if (p.first != key) filtered.push_back(p);
    return to_wire(filtered);
}

DecodeResult decode(const std::string &text) { return TelemetryDecoder{}.decode(text, 1000); }

RawSnapshot valid_raw(uint64_t seq, int pid, int64_t received) {
    auto r = decode(with("sequence", std::to_string(seq)));
    r.snapshot.daemon_pid = pid;
    r.snapshot.received_monotonic_ms = received;
    return r.snapshot;
}

} // namespace

// --- Decoding --------------------------------------------------------------

TEST("decode: a valid schema-v2 snapshot decodes with correct fields") {
    auto r = decode(to_wire(canonical()));
    CHECK(r.ok());
    CHECK_EQ(r.snapshot.schema_version, 2);
    CHECK_EQ(r.snapshot.sequence, 42u);
    CHECK_EQ(r.snapshot.focused_package, std::string("com.example.game"));
    CHECK(r.snapshot.has_thermal());
    CHECK_NEAR(r.snapshot.thermal_headroom, 0.42f, 0.001f);
    CHECK_EQ(r.snapshot.received_monotonic_ms, 1000);
}

TEST("decode: an unsupported schema version is rejected") {
    CHECK(decode(with("schema_version", "3")).error == DecodeError::UnsupportedSchema);
    CHECK(decode(with("schema_version", "1")).error == DecodeError::UnsupportedSchema);
}

TEST("decode: a missing schema version is rejected") {
    CHECK(decode(without("schema_version")).error == DecodeError::MissingSchemaVersion);
}

TEST("decode: a missing required field is rejected with context") {
    auto r = decode(without("battery_saver"));
    CHECK(r.error == DecodeError::MissingRequiredField);
    CHECK_EQ(r.detail, std::string("battery_saver"));
}

TEST("decode: an unknown optional field is ignored") {
    CHECK(decode(with("future_optional_key", "whatever")).ok());
}

TEST("decode: a malformed integer is rejected") {
    CHECK(decode(with("sequence", "12x3")).error == DecodeError::MalformedInteger);
}

TEST("decode: a malformed float is rejected") {
    CHECK(decode(with("thermal_headroom", "0.4-2")).error == DecodeError::MalformedFloat);
}

TEST("decode: NaN headroom is rejected when the sample claims to be valid") {
    CHECK(decode(with("thermal_headroom", "nan")).error == DecodeError::NonFiniteValue);
}

TEST("decode: NaN headroom is accepted when the sample is marked invalid") {
    auto kv = canonical();
    for (auto &p : kv) {
        if (p.first == "thermal_valid") p.second = "0";
        if (p.first == "thermal_headroom") p.second = "nan";
    }
    auto r = decode(to_wire(kv));
    CHECK(r.ok());
    CHECK(!r.snapshot.has_thermal());
}

TEST("decode: an infinite headroom is always rejected") {
    CHECK(decode(with("thermal_headroom", "inf")).error == DecodeError::NonFiniteValue);
}

TEST("decode: a headroom above 1.0 is preserved, not clamped") {
    auto r = decode(with("thermal_headroom", "1.30"));
    CHECK(r.ok());
    CHECK_NEAR(r.snapshot.thermal_headroom, 1.30f, 0.001f);
}

TEST("decode: the full integer zen mode is preserved") {
    auto r = decode(with("zen_mode", "2"));
    CHECK(r.ok());
    CHECK_EQ(r.snapshot.zen_mode, 2);
}

TEST("decode: a duplicate key is rejected") {
    CHECK(decode(to_wire(canonical()) + "sequence 99\n").error == DecodeError::DuplicateKey);
}

TEST("decode: a negative pid is rejected") {
    CHECK(decode(with("focused_pid", "-1")).error == DecodeError::ImplausibleIdentifier);
}

TEST("decode: empty input is rejected") {
    CHECK(decode("").error == DecodeError::EmptyInput);
}

TEST("decode: oversized input is rejected before allocation") {
    CHECK(decode(std::string(70000, 'a')).error == DecodeError::InputTooLarge);
}

TEST("decode: an over-long line is rejected") {
    CHECK(decode("schema_version " + std::string(5000, '1') + "\n").error == DecodeError::LineTooLong);
}

TEST("decode: too many lines are rejected") {
    std::string big;
    for (int i = 0; i < 300; ++i) big += "pad" + std::to_string(i) + " 1\n";
    CHECK(decode(big).error == DecodeError::TooManyLines);
}

TEST("decode: a failed decode leaves no partial snapshot") {
    auto r = decode(with("focused_uid", "-5"));
    CHECK(!r.ok());
    CHECK_EQ(r.snapshot.schema_version, 0); // default-constructed, nothing carried over
    CHECK_EQ(r.snapshot.sequence, 0u);
}

// --- Freshness / health ----------------------------------------------------

TEST("freshness: unavailable before anything is observed") {
    TelemetryFreshness f;
    CHECK(f.health(0) == TelemetryHealth::Unavailable);
}

TEST("freshness: a single fresh sample is Delayed until it stabilizes") {
    TelemetryFreshness f;
    f.observe(valid_raw(1, 100, 1000), 1000);
    CHECK(f.health(1000) == TelemetryHealth::Delayed); // consecutive 1 < stabilize 2
    f.observe(valid_raw(2, 100, 1200), 1200);
    CHECK(f.health(1200) == TelemetryHealth::Healthy);
}

TEST("freshness: ages through Delayed, Stale, Unavailable") {
    TelemetryFreshness f;
    f.observe(valid_raw(1, 100, 0), 0);
    f.observe(valid_raw(2, 100, 100), 100); // stabilized
    CHECK(f.health(100) == TelemetryHealth::Healthy);
    CHECK(f.health(4000) == TelemetryHealth::Delayed);
    CHECK(f.health(6000) == TelemetryHealth::Stale);
    CHECK(f.health(20000) == TelemetryHealth::Unavailable);
}

TEST("freshness: an unsupported-schema decode reports UnsupportedSchema") {
    TelemetryFreshness f;
    f.observe_failure(DecodeError::UnsupportedSchema, 1000);
    CHECK(f.health(1000) == TelemetryHealth::UnsupportedSchema);
}

TEST("freshness: a first-ever invalid decode reports Invalid") {
    TelemetryFreshness f;
    f.observe_failure(DecodeError::MalformedFloat, 1000);
    CHECK(f.health(1000) == TelemetryHealth::Invalid);
}

TEST("freshness: a duplicate sequence is flagged and does not stabilize") {
    TelemetryFreshness f;
    f.observe(valid_raw(7, 100, 0), 0);
    f.observe(valid_raw(7, 100, 100), 100); // same sequence
    CHECK(f.last_was_duplicate());
    CHECK(f.health(100) == TelemetryHealth::Delayed); // still not stabilized
}

TEST("freshness: a sequence regression under the same pid is flagged") {
    TelemetryFreshness f;
    f.observe(valid_raw(9, 100, 0), 0);
    f.observe(valid_raw(4, 100, 100), 100);
    CHECK(f.last_was_regression());
}

TEST("freshness: a daemon restart (new pid, reset sequence) is not corruption") {
    TelemetryFreshness f;
    f.observe(valid_raw(500, 100, 0), 0);
    f.observe(valid_raw(1, 200, 100), 100); // new pid, sequence reset
    CHECK(!f.last_was_regression());
    CHECK_EQ(f.restart_count(), 1u);
}

TEST("freshness: after a long gap, promotion requires re-stabilization") {
    TelemetryFreshness f;
    f.observe(valid_raw(1, 100, 0), 0);
    f.observe(valid_raw(2, 100, 100), 100);
    CHECK(f.health(100) == TelemetryHealth::Healthy);
    // Long gap, then telemetry returns: the first sample back must not be Healthy.
    f.observe(valid_raw(3, 100, 30000), 30000);
    CHECK(f.health(30000) == TelemetryHealth::Delayed);
    f.observe(valid_raw(4, 100, 30100), 30100);
    CHECK(f.health(30100) == TelemetryHealth::Healthy);
}

// --- Assembler -------------------------------------------------------------

TEST("assembler: maps health onto the engine's DataHealth") {
    using AH = RuntimeSnapshotAssembler;
    CHECK(AH::to_data_health(TelemetryHealth::Healthy) == flux::engine::DataHealth::Healthy);
    CHECK(AH::to_data_health(TelemetryHealth::Delayed) == flux::engine::DataHealth::Stale);
    CHECK(AH::to_data_health(TelemetryHealth::Stale) == flux::engine::DataHealth::Stale);
    CHECK(AH::to_data_health(TelemetryHealth::Invalid) == flux::engine::DataHealth::Offline);
    CHECK(AH::to_data_health(TelemetryHealth::Unavailable) == flux::engine::DataHealth::Offline);
    CHECK(AH::to_data_health(TelemetryHealth::UnsupportedSchema) == flux::engine::DataHealth::Offline);
}

TEST("assembler: a valid thermal reading passes through; an absent one is nullopt") {
    RuntimeSnapshotAssembler a;
    RawSnapshot raw = decode(with("thermal_headroom", "0.90")).snapshot;
    auto out = a.assemble(raw, TelemetryHealth::Healthy, ForegroundResolution{});
    CHECK(out.runtime.thermal.has_value());
    CHECK(out.capabilities.thermal_supported);

    RawSnapshot novalid = raw;
    novalid.thermal_valid = false;
    auto out2 = a.assemble(novalid, TelemetryHealth::Healthy, ForegroundResolution{});
    CHECK(!out2.runtime.thermal.has_value());
}

TEST("assembler: unavailable providers degrade to safe defaults") {
    RuntimeSnapshotAssembler a;
    RawSnapshot raw = decode(to_wire(canonical())).snapshot;
    raw.screen_available = false;
    raw.power_available = false;
    raw.battery_saver = true; // must be ignored: power unavailable
    auto out = a.assemble(raw, TelemetryHealth::Healthy, ForegroundResolution{});
    CHECK(out.runtime.screen_awake);      // unknown screen -> assumed awake
    CHECK(!out.runtime.battery_saver);    // unavailable -> not asserted
}

TEST("assembler: foreground falls back to SynthesisCore, or reports unavailable") {
    RawSnapshot raw = decode(to_wire(canonical())).snapshot;
    auto fg = RuntimeSnapshotAssembler::foreground_from_synthesiscore(raw);
    CHECK(fg.source == ForegroundProvider::SynthesisCoreFallback);
    CHECK_EQ(fg.info.package, std::string("com.example.game"));

    raw.foreground_available = false;
    auto none = RuntimeSnapshotAssembler::foreground_from_synthesiscore(raw);
    CHECK(none.source == ForegroundProvider::Unavailable);
}

TEST("assembler: a native provider wins the provider-neutral selection") {
    RawSnapshot raw = decode(to_wire(canonical())).snapshot;
    ForegroundResolution native;
    native.source = ForegroundProvider::NativeProcessEventSource;
    native.info.present = true;
    native.info.package = "com.native.detected";
    auto chosen = RuntimeSnapshotAssembler::select_foreground(native, raw);
    CHECK(chosen.source == ForegroundProvider::NativeProcessEventSource);

    auto fallback = RuntimeSnapshotAssembler::select_foreground(std::nullopt, raw);
    CHECK(fallback.source == ForegroundProvider::SynthesisCoreFallback);
}

// --- Store -----------------------------------------------------------------

TEST("store: publish then get returns a complete copy and advances generation") {
    TelemetryStore store;
    CHECK(!store.get().has_value());
    store.publish(valid_raw(1, 100, 1000), TelemetryHealth::Healthy);
    auto p = store.get();
    CHECK(p.has_value());
    CHECK_EQ(p->snapshot.sequence, 1u);
    CHECK(p->health == TelemetryHealth::Healthy);
    CHECK_EQ(p->generation, 1u);

    store.publish_health(TelemetryHealth::Stale);
    CHECK(store.get()->health == TelemetryHealth::Stale);
    CHECK_EQ(store.get()->snapshot.sequence, 1u); // snapshot unchanged
}

TEST("store: concurrent readers never observe a torn snapshot") {
    TelemetryStore store;
    // Two fully-distinct states; a reader must always see one whole state, never a mixture.
    RawSnapshot a = valid_raw(100, 100, 0);
    a.focused_package = "com.state.a";
    a.focused_pid = 111;
    RawSnapshot b = valid_raw(200, 200, 0);
    b.focused_package = "com.state.b";
    b.focused_pid = 222;
    store.publish(a, TelemetryHealth::Healthy);

    std::atomic<bool> stop{false};
    std::atomic<bool> torn{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                auto p = store.get();
                if (!p) continue;
                const bool is_a = p->snapshot.sequence == 100 &&
                                  p->snapshot.focused_package == "com.state.a" &&
                                  p->snapshot.focused_pid == 111;
                const bool is_b = p->snapshot.sequence == 200 &&
                                  p->snapshot.focused_package == "com.state.b" &&
                                  p->snapshot.focused_pid == 222;
                if (!is_a && !is_b) torn.store(true);
            }
        });
    }
    for (int i = 0; i < 20000; ++i) store.publish((i & 1) ? b : a, TelemetryHealth::Healthy);
    stop.store(true);
    for (auto &t : readers) t.join();
    CHECK(!torn.load());
}
