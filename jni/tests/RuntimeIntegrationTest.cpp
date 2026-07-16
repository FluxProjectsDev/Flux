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

// Live-migration tests: the AtomicStatusWatcher against a real inotify + temp directory,
// the TelemetryIngestor's malformed-safety, and the full V2 runtime data flow
// (watcher/ingestor -> store -> assembler -> DecisionEngine -> ExecutionPlanner ->
// ExecutionEngine) with injectable fakes. No root, no device, no NDK.

#include "TestFramework.hpp"

#include "AtomicStatusWatcher.hpp"
#include "RuntimeSnapshotAssembler.hpp"
#include "TelemetryIngestor.hpp"
#include "TelemetryStore.hpp"

#include "DecisionEngine.hpp"
#include "ExecutionEngine.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

using namespace std::chrono_literals;
namespace tel = flux::telemetry;
namespace eng = flux::engine;
namespace exe = flux::execution;

namespace {

std::string valid_wire(uint64_t seq, float headroom, int status, bool battery, bool screen,
                       const std::string &pkg) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(headroom));
    std::string h = buf;
    std::string out;
    auto add = [&](const std::string &k, const std::string &v) { out += k + " " + v + "\n"; };
    add("schema_version", "2");
    add("sequence", std::to_string(seq));
    add("updated_elapsed_ms", "100000");
    add("daemon_pid", "1234");
    add("foreground_available", "1");
    add("focused_package", pkg);
    add("focused_pid", "5678");
    add("focused_uid", "10123");
    add("screen_available", "1");
    add("screen_awake", screen ? "1" : "0");
    add("power_available", "1");
    add("battery_saver", battery ? "1" : "0");
    add("charging_available", "1");
    add("charging_state", "0");
    add("thermal_available", "1");
    add("thermal_valid", "1");
    add("thermal_headroom", h);
    add("thermal_status", std::to_string(status));
    add("thermal_sample_elapsed_ms", "99500");
    add("thermal_age_ms", "500");
    add("audio_available", "1");
    add("audio_active", "0");
    add("zen_available", "1");
    add("zen_mode", "0");
    add("kernel_is_gki", "1");
    return out;
}

struct TempDir {
    std::filesystem::path dir;
    TempDir() {
        char tmpl[] = "/tmp/flux-watcher-XXXXXX";
        dir = ::mkdtemp(tmpl);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

void atomic_write(const std::filesystem::path &dir, const std::string &name, const std::string &data) {
    const auto tmp = dir / (name + ".tmp");
    const auto target = dir / name;
    FILE *f = std::fopen(tmp.c_str(), "wb");
    std::fwrite(data.data(), 1, data.size(), f);
    std::fflush(f);
    std::fclose(f);
    std::filesystem::rename(tmp, target); // atomic replace: delivers IN_MOVED_TO
}

struct Sink {
    std::mutex m;
    int calls = 0;
    std::optional<std::string> last;
    bool last_uncertain = false;
    void operator()(std::optional<std::string> c, bool u) {
        std::lock_guard<std::mutex> lk(m);
        ++calls;
        last = std::move(c);
        last_uncertain = u;
    }
    int count() {
        std::lock_guard<std::mutex> lk(m);
        return calls;
    }
    std::optional<std::string> content() {
        std::lock_guard<std::mutex> lk(m);
        return last;
    }
};

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds budget = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

} // namespace

// --- AtomicStatusWatcher ---------------------------------------------------

TEST("watcher: delivers an already-present file on start (reconciliation)") {
    TempDir t;
    atomic_write(t.dir, "status", valid_wire(1, 0.3f, 0, false, true, "com.g"));
    Sink sink;
    tel::AtomicStatusWatcher w(t.dir.string(), "status",
                               [&](auto c, bool u) { sink(std::move(c), u); });
    CHECK(w.start());
    CHECK(wait_until([&] { return sink.count() >= 1; }));
    CHECK(sink.content().has_value());
    w.stop();
}

TEST("watcher: an atomic rename is delivered via IN_MOVED_TO") {
    TempDir t;
    Sink sink;
    tel::AtomicStatusWatcher w(t.dir.string(), "status",
                               [&](auto c, bool u) { sink(std::move(c), u); });
    CHECK(w.start()); // no file yet: initial delivery is nullopt
    const int before = sink.count();
    atomic_write(t.dir, "status", valid_wire(7, 0.9f, 0, false, true, "com.g"));
    CHECK(wait_until([&] { return sink.count() > before && sink.content().has_value(); }));
    CHECK(sink.content()->find("sequence 7") != std::string::npos);
    w.stop();
}

TEST("watcher: target deletion then recreation are both observed") {
    TempDir t;
    atomic_write(t.dir, "status", valid_wire(1, 0.3f, 0, false, true, "com.g"));
    Sink sink;
    tel::AtomicStatusWatcher w(t.dir.string(), "status",
                               [&](auto c, bool u) { sink(std::move(c), u); });
    CHECK(w.start());
    CHECK(wait_until([&] { return sink.content().has_value(); }));

    std::filesystem::remove(t.dir / "status");
    CHECK(wait_until([&] {
        auto c = sink.content();
        return !c.has_value();
    }));

    atomic_write(t.dir, "status", valid_wire(2, 0.3f, 0, false, true, "com.g"));
    CHECK(wait_until([&] {
        auto c = sink.content();
        return c.has_value() && c->find("sequence 2") != std::string::npos;
    }));
    w.stop();
}

TEST("watcher: rapid replacements all land, ending on the newest") {
    TempDir t;
    Sink sink;
    tel::AtomicStatusWatcher w(t.dir.string(), "status",
                               [&](auto c, bool u) { sink(std::move(c), u); });
    CHECK(w.start());
    for (uint64_t i = 1; i <= 50; ++i)
        atomic_write(t.dir, "status", valid_wire(i, 0.3f, 0, false, true, "com.g"));
    CHECK(wait_until([&] {
        auto c = sink.content();
        return c.has_value() && c->find("sequence 50") != std::string::npos;
    }));
    w.stop();
}

TEST("watcher: stops cleanly while replacements are in flight") {
    TempDir t;
    Sink sink;
    auto w = std::make_unique<tel::AtomicStatusWatcher>(
        t.dir.string(), "status", [&](auto c, bool u) { sink(std::move(c), u); });
    CHECK(w->start());
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        uint64_t i = 1;
        while (!stop.load()) atomic_write(t.dir, "status", valid_wire(i++, 0.3f, 0, false, true, "g"));
    });
    std::this_thread::sleep_for(50ms);
    w->stop();   // must join deterministically, no hang, no use-after-close
    stop.store(true);
    writer.join();
    w.reset();
    CHECK(true); // reaching here without deadlock/crash is the assertion
}

// --- TelemetryIngestor -----------------------------------------------------

TEST("ingestor: a malformed replacement does not destroy the last good snapshot") {
    tel::TelemetryStore store;
    tel::TelemetryIngestor ing(store);
    ing.ingest(valid_wire(1, 0.3f, 0, false, true, "com.g"), false, 0);
    ing.ingest(valid_wire(2, 0.3f, 0, false, true, "com.g"), false, 100);
    CHECK(store.get().has_value());
    CHECK_EQ(store.get()->snapshot.sequence, 2u);

    ing.ingest(std::string("schema_version 2\ngarbage line without value valuevalue\n"), false, 200);
    auto p = store.get();
    CHECK(p.has_value());
    CHECK_EQ(p->snapshot.sequence, 2u); // previous good snapshot survives
}

TEST("ingestor: absent content keeps the last snapshot and ages its health") {
    tel::TelemetryStore store;
    tel::TelemetryIngestor ing(store);
    ing.ingest(valid_wire(1, 0.3f, 0, false, true, "com.g"), false, 0);
    ing.ingest(valid_wire(2, 0.3f, 0, false, true, "com.g"), false, 100);
    CHECK(store.get()->health == tel::TelemetryHealth::Healthy);

    ing.ingest(std::nullopt, false, 100);
    CHECK_EQ(store.get()->snapshot.sequence, 2u);
    ing.tick(30000); // long gap
    CHECK(store.get()->health == tel::TelemetryHealth::Unavailable);
}

// --- Full runtime integration ----------------------------------------------

namespace {

// A minimal device profile mapping for the integration flow.
exe::ProfilePlanSpec spec_for(eng::TargetProfile p) {
    std::string gov;
    switch (p) {
        case eng::TargetProfile::Performance: gov = "performance"; break;
        case eng::TargetProfile::PerformanceLite: gov = "schedutil"; break;
        case eng::TargetProfile::Balanced: gov = "schedutil"; break;
        case eng::TargetProfile::PowerSave: gov = "powersave"; break;
    }
    return exe::ProfilePlanSpec{{{"cpu.governor", gov, eng::target_profile_name(p)}}};
}

exe::NodeDescriptor governor() {
    exe::NodeDescriptor d;
    d.id = "cpu.governor";
    d.path = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor";
    d.readable = d.writable = true;
    d.type = exe::ValueType::Enum;
    d.allowed = {"performance", "schedutil", "powersave"};
    d.critical = true;
    return d;
}

// Drive raw content through ingestor->store->assembler->engine->planner->executor.
struct Runtime {
    tel::TelemetryStore store;
    tel::TelemetryIngestor ingestor{store};
    tel::RuntimeSnapshotAssembler assembler;
    eng::DecisionEngine engine;
    eng::EngineState state;
    exe::InMemoryNodeBackend backend;
    exe::CapabilityRegistry registry;
    exe::ExecutionPlanner planner;
    exe::ExecutionEngine executor{backend};

    Runtime() {
        backend.seed(governor().path, "schedutil");
        registry.register_node(governor(), backend);
        state.current = eng::TargetProfile::Balanced;
        state.prev_health = eng::DataHealth::Healthy; // avoid restore-settle in these steady tests
        state.prev_in_session = true;
    }

    exe::ApplyResult step(const std::string &wire, int64_t now) {
        ingestor.ingest(wire, false, now);
        return evaluate(now);
    }

    // Evaluate against the currently-published telemetry (no new ingest).
    exe::ApplyResult evaluate(int64_t now) {
        auto pub = store.get();
        auto assembled = assembler.assemble(pub->snapshot, pub->health,
                                            tel::RuntimeSnapshotAssembler::foreground_from_synthesiscore(
                                                pub->snapshot));
        eng::DecisionInputs in;
        in.runtime = assembled.runtime;
        in.capabilities = assembled.capabilities;
        in.session.in_session = assembled.foreground.info.present;
        in.session.package = assembled.foreground.info.package;
        eng::Decision d = engine.evaluate(in, state, now);
        state = d.next_state;
        exe::ExecutionPlan plan = planner.plan(spec_for(d.desired_profile), registry, backend);
        return executor.apply(plan, eng::target_profile_name(d.desired_profile),
                              eng::target_profile_name(state.current), eng::decision_reason_name(d.reason),
                              now);
    }

    // Cool game to steady Performance: telemetry needs 2 samples to be Healthy, the engine
    // then defers one more cycle (restore-settle) before an aggressive promotion.
    void warm_to_performance() {
        step(valid_wire(1, 0.30f, 0, false, true, "com.g"), 0);
        step(valid_wire(2, 0.30f, 0, false, true, "com.g"), 100);
        step(valid_wire(3, 0.30f, 0, false, true, "com.g"), 200);
    }
};

} // namespace

TEST("integration: a cool game session drives a verified Performance apply") {
    Runtime rt;
    rt.warm_to_performance();
    CHECK(rt.state.current == eng::TargetProfile::Performance);
    CHECK_EQ(rt.backend.read(governor().path).value(), std::string("performance"));
}

TEST("integration: a thermal emergency drives a safe apply regardless of session") {
    Runtime rt;
    rt.warm_to_performance();
    exe::ApplyResult r = rt.step(valid_wire(4, 1.30f, 4, false, true, "com.g"), 300); // emergency
    CHECK(r.verified_active);
    CHECK_EQ(rt.backend.read(governor().path).value(), std::string("schedutil")); // Balanced
}

TEST("integration: stale telemetry never promotes performance") {
    Runtime rt;
    // One cool sample: telemetry is Delayed/Stale, never Healthy, so the engine must not
    // raise the profile above the safe Balanced tier.
    rt.step(valid_wire(1, 0.30f, 0, false, true, "com.g"), 0);
    rt.ingestor.tick(6000); // age it to Stale
    exe::ApplyResult r = rt.evaluate(6000);
    CHECK(rt.store.get()->health == tel::TelemetryHealth::Stale);
    CHECK(rt.state.current != eng::TargetProfile::Performance);
    CHECK_EQ(rt.backend.read(governor().path).value(), std::string("schedutil"));
    (void)r;
}

TEST("integration: repeating the same decision performs no extra writes") {
    Runtime rt;
    rt.warm_to_performance();
    const int writes = rt.backend.write_count(governor().path);
    exe::ApplyResult again = rt.step(valid_wire(4, 0.30f, 0, false, true, "com.g"), 300);
    CHECK_EQ(again.skipped_idempotent, 1);
    CHECK_EQ(rt.backend.write_count(governor().path), writes); // unchanged
}
