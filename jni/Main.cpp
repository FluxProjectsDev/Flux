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

#include <atomic>
#include <chrono>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

#include <poll.h>
#include <sys/eventfd.h>

#include "DeviceMitigationStore.hpp"
#include <DeviceInfo.hpp>
#include "FluxCLI.hpp"
#include "FluxConfigStore.hpp"
#include "InotifyHandler.hpp"
#include "Write2File.hpp"

#include <AndroidZenBackend.hpp>         // the single zen write entry point
#include <DecisionAdapter.hpp>          // Flux V2 Decision Engine (pulls in ProfilePolicy.hpp types)
#include <DevicePacks.hpp>              // attributed, gated per-SoC capability data
#include <ExecutionRuntime.hpp>         // the single live profile-apply entry point
#include <RuntimeSnapshotAssembler.hpp>  // normalization boundary: RawSnapshot -> RuntimeSnapshot
#include <TelemetryRuntime.hpp>          // the single live telemetry authority

#include <Flux.hpp>
#include <FluxLog.hpp>
#include <FluxUtility.hpp>
#include <GameRegistry.hpp>
#include <LockFile.hpp>
#include <ModuleProperty.hpp>
#include <PIDTracker.hpp>
#include <ShellUtility.hpp>
#include <SignalHandler.hpp>

// ---------------------------------------------------------------------------
// Global registry
// ---------------------------------------------------------------------------

GameRegistry game_registry;

// ---------------------------------------------------------------------------
// module.prop management
// ---------------------------------------------------------------------------

void set_module_description_status(const std::string &status) {
    const std::string description = "[" + status + "] Special performance module for your Device.";
    const std::vector<ModuleProperties> props{{"description", description}};
    try {
        ModuleProperty::Change(MODULE_PROP, props);
    } catch (const std::runtime_error &e) {
        LOGE("Failed to apply module properties: {}", e.what());
    }
}

void notify_fatal_error(const std::string &error_msg) {
    notify(("ERROR: " + error_msg).c_str());
    set_module_description_status("\xE2\x9D\x8C " + error_msg);
}

// ---------------------------------------------------------------------------
// Global event signaling for immediate daemon wake-up
// ---------------------------------------------------------------------------

int synthesis_core_event_fd = -1;

/// The one and only live telemetry authority: watcher -> ingestor -> decoder -> freshness ->
/// store. Constructed in run_daemon() once the wake path exists, because its delivery callback
/// signals the daemon. Nothing else in the process may hold telemetry state.
static std::unique_ptr<flux::telemetry::TelemetryRuntime> telemetry_runtime;

/// The one and only live execution authority: intents -> plan -> verified writes. Constructed
/// after telemetry (its zen backend reads through it) and destroyed before it.
///
/// This is the daemon's sole profile-apply entry point. Main.cpp routes events to it and reads
/// state from it; it does not write device nodes, does not shell out, and does not decide.
static std::unique_ptr<flux::execution::ExecutionRuntime> execution_runtime;

/// The production zen backend, owned here because the runtime borrows it. Reads the live mode
/// from telemetry rather than asking the system a second time.
static std::unique_ptr<flux::execution::AndroidZenBackend> zen_backend;

std::atomic<bool> daemon_stop_requested{false};

/**
 * @brief Signal the daemon to wake pool loop
 *
 * Uses a local snapshot of synthesis_core_event_fd to avoid a TOCTOU race
 * where another thread closes the fd between the ">= 0" check and the write().
 */
void signal_daemon_update() {
    const int fd = synthesis_core_event_fd; // atomic snapshot
    if (fd >= 0) {
        uint64_t val = 1;
        ssize_t ret = write(fd, &val, sizeof(val));
        (void)ret; // Suppress unused warning
    }
}

/// Defined below, once the config stores and the runtime are in scope.
static void install_tuning_from_config(int64_t now_ms);

/**
 * @brief Tell the execution runtime its capability assumptions may be stale.
 *
 * Called by the file watcher when configuration changes. The engine's idempotency cache skips a
 * write when it believes the value is already in place; that belief was formed under the old
 * configuration, so it has to be dropped rather than trusted. Also wakes the loop, so the
 * re-apply happens now instead of on the next tick.
 */
void invalidate_execution_capabilities(const char *reason) {
    if (!execution_runtime) return; // config can change before the runtime exists
    LOGD("Execution capabilities invalidated: {}", reason);
    // Re-migrate rather than merely invalidating: the change may have been to a setting the
    // tuning captured (a governor, a mitigation, the master switch), and re-reading it is the
    // only way the new value reaches the descriptors. set_tuning() bumps the generation itself.
    install_tuning_from_config(flux_monotonic_ms());
    signal_daemon_update();
}

/**
 * @brief Signal the daemon to stop immediately
 */
void signal_daemon_stop() {
    daemon_stop_requested.store(true, std::memory_order_relaxed);
    signal_daemon_update();
}

// ---------------------------------------------------------------------------
// Lock file management
// ---------------------------------------------------------------------------

static LockFile daemon_lock{LOCK_FILE};
static LockFile java_lock{JAVA_LOCK_FILE};

/**
 * @brief Acquire the daemon singleton lock (non-blocking)
 *
 * Creates LOCK_FILE if absent and tries F_SETLK. Returns false (and leaves
 * the file unlocked) when another daemon instance is already running.
 */
[[nodiscard]] bool create_lock_file() {
    return daemon_lock.acquire(LockFile::AcquireMode::NonBlocking, LockFile::LockType::Exclusive);
}

/// Set when SynthesisCore's lock goes away, i.e. the telemetry producer died.
/// Read by the main loop; cleared when a fresh snapshot proves it is back.
std::atomic<bool> synthesiscore_down{false};

/// How many times we have observed SynthesisCore disappear. Exposed to diagnostics.
std::atomic<uint64_t> synthesiscore_restart_count{0};

/**
 * @brief Arm the SynthesisCore liveness watch.
 *
 * Installs a LockFile::watch() on JAVA_LOCK_FILE, which SynthesisCore holds for as long as
 * it is alive.
 *
 * **This no longer stops the daemon.** It used to call signal_daemon_stop(), so a crash in
 * the telemetry producer terminated the entire policy engine — a sensor failure took down
 * the thing that reacts to sensors, and the device was left running whatever profile
 * happened to be applied at that moment, with nothing left to change it.
 *
 * Flux now records the outage, falls back to a safe profile through the normal policy path
 * (telemetry health becomes Offline), and keeps running. The service supervisor restarts
 * SynthesisCore; when a healthy v2 snapshot appears, the policy resumes on its own.
 */
void watch_java_lock() {
    java_lock.watch([] {
        // Fires only on the free transition: SynthesisCore released its lock, i.e.
        // the telemetry producer exited.
        synthesiscore_down.store(true, std::memory_order_relaxed);
        synthesiscore_restart_count.fetch_add(1, std::memory_order_relaxed);
        LOGW("SynthesisCore lock released: telemetry producer exited. "
             "Falling back to a safe profile and awaiting its restart.");
        set_module_description_status("\xE2\x9A\xA0 Telemetry unavailable, running safe profile");
        signal_daemon_update(); // re-evaluate the policy now, do not wait for a tick
    });
}

// ---------------------------------------------------------------------------
// Daemon States
// ---------------------------------------------------------------------------

struct DaemonState {
    /// All profile-selection logic lives in the Flux V2 Decision Engine (pure, host-tested),
    /// reached through FluxDecisionService. This struct holds only what the daemon needs to
    /// *drive* it and to act on its output. The legacy ProfilePolicy is no longer on the
    /// runtime decision path.
    FluxDecisionService decision_service{};
    PolicyState policy_state{};
    TransitionHistory history{64};

    std::string active_package;
    bool in_game_session = false;
    int focus_loss_count = 0;

    // Zen state deliberately absent. The user's original mode, whether Flux is holding zen, and
    // the "do not read our own write back" guard all live in the ZenController the execution
    // runtime owns. Keeping a second copy here is what let the daemon and the controller
    // disagree about what the user's zen mode had been.

    PIDTracker pid_tracker;
};

/// How often the daemon re-evaluates even when nothing has happened.
///
/// The loop is event-driven, but a dead producer generates no events — so a purely
/// event-driven loop would block in poll() forever and never notice that its telemetry had
/// gone stale. This tick is what makes the staleness and offline transitions actually fire.
static constexpr int DAEMON_TICK_MS = 1000;

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------


/**
 * @brief The SoC family, from the code the installer already worked out.
 *
 * Read once, at install time, into `soc_recognition` by customize.sh. Flux does not re-derive it
 * at runtime: the installer's answer is the one the rest of the module (including the utility
 * script) already agrees with, and a second detector could disagree with the first.
 *
 * SocFamily's numeric values are deliberately the same codes, so this is a checked cast rather
 * than a mapping table that could drift.
 */
[[nodiscard]] static flux::execution::SocFamily read_soc_family() {
    std::ifstream file(CONFIG_DIR "/soc_recognition");
    if (!file.is_open()) return flux::execution::SocFamily::Unknown;

    int code = 0;
    if (!(file >> code)) return flux::execution::SocFamily::Unknown;

    switch (static_cast<flux::execution::SocFamily>(code)) {
        case flux::execution::SocFamily::MediaTek:
        case flux::execution::SocFamily::Snapdragon:
        case flux::execution::SocFamily::Exynos:
        case flux::execution::SocFamily::Unisoc:
        case flux::execution::SocFamily::Tensor:
        case flux::execution::SocFamily::Tegra:
        case flux::execution::SocFamily::Generic:
            return static_cast<flux::execution::SocFamily>(code);
        case flux::execution::SocFamily::Unknown:
            break;
    }
    // An unrecognised code means the generic pack only. Guessing a family is the one mistake
    // the whole descriptor design exists to prevent.
    return flux::execution::SocFamily::Unknown;
}

/**
 * @brief Build the V2 tuning from stored configuration, and install it.
 *
 * The migration boundary. Settings the legacy shell read from FLUX_* environment variables are
 * turned into semantic inputs here — governor choices, suppressed capabilities, and the master
 * gate — and handed to the runtime, which expresses them as descriptor values rather than as
 * commands. Nothing on this path can execute anything.
 */
static void install_tuning_from_config(int64_t now_ms) {
    if (!execution_runtime) return;

    const auto prefs = config_store.get_preferences();
    const auto governors = config_store.get_cpu_governor();

    flux::execution::LegacyConfigInput input;
    input.disable_tweaks = prefs.disable_tweaks;
    input.balanced_governor = governors.balance;
    input.powersave_governor = governors.powersave;

    // The mitigation set is device knowledge plus a user opt-in; the migration decides what each
    // item means. Main.cpp does not interpret them.
    const auto items = device_mitigation_store.get_cached_mitigation_items(prefs.use_device_mitigation);
    input.mitigation_items.insert(items.begin(), items.end());

    auto tuning = flux::execution::migrate_legacy_config(input);
    for (const auto &note : tuning.notes) {
        LOGI("config: {} -> {} ({})", note.key,
             flux::execution::migration_outcome_name(note.outcome), note.detail);
    }
    execution_runtime->set_tuning(std::move(tuning), now_ms);
}

/**
 * @brief The profile mode to publish for the WebUI, from the *verified* runtime profile.
 *
 * Nothing is claimed before an apply verifies. Until then the published profile is the safe
 * default rather than the one policy hopes to reach.
 */
[[nodiscard]] static FluxProfileMode profile_mode_from_target(flux::engine::TargetProfile profile,
                                                              bool verified) {
    if (!verified) return BALANCE_PROFILE;
    switch (profile) {
        case flux::engine::TargetProfile::Performance: return PERFORMANCE_PROFILE;
        case flux::engine::TargetProfile::PerformanceLite: return PERFORMANCE_LITE_PROFILE;
        case flux::engine::TargetProfile::Balanced: return BALANCE_PROFILE;
        case flux::engine::TargetProfile::PowerSave: return POWERSAVE_PROFILE;
    }
    return BALANCE_PROFILE;
}

/**
 * @brief Returns the package name of the focused registered game, or empty if none.
 */
[[nodiscard]] static std::string get_active_game(const flux::telemetry::RawSnapshot &snap,
                                                GameRegistry &registry) {
    if (!snap.foreground_available) return {};
    if (registry.is_game_registered(snap.focused_package)) {
        return snap.focused_package;
    }
    return {};
}

/**
 * @brief Returns true if the active game still holds focus.
 *
 * Uses a 3-strike debounce so transient focus blips (in-game overlays, permission dialogs)
 * are not mistaken for a genuine exit. Process death is handled by the PID tracker callback,
 * so this only examines focus.
 */
[[nodiscard]] static bool is_game_still_active(DaemonState &state,
                                              const flux::telemetry::RawSnapshot &snap) {
    if (snap.focused_package == state.active_package) {
        state.focus_loss_count = 0;
        return true;
    }

    state.focus_loss_count++;
    if (state.focus_loss_count < 3) {
        LOGD("Focus lost for {}, strike {}/3", state.active_package, state.focus_loss_count);
        return true;
    }

    LOGD("Game {} no longer focused (3 strikes)", state.active_package);
    state.focus_loss_count = 0;
    return false;
}

/**
 * @brief Returns the PID of @p package_name, or 0 on failure.
 */
[[nodiscard]] static pid_t pidof_game(const std::string &package_name,
                                      const std::optional<flux::telemetry::RawSnapshot> &snap) {
    if (snap && snap->focused_package == package_name && snap->focused_pid > 0) {
        return static_cast<pid_t>(snap->focused_pid);
    }

    const pid_t pid = pidof(package_name, false);
    if (pid != 0) return pid;

    LOGE_TAG("pidof_game", "Could not find PID for {}", package_name);
    return 0;
}

/**
 * @brief Handle the tracked game process exiting.
 *
 * Zen restoration is the execution runtime's job now: it holds the exact original mode and the
 * only zen write path. This used to call restore_zen_if_needed(), a second zen writer living in
 * Main.cpp beside the one in the runtime.
 */
static void handle_game_exit(DaemonState &state) {
    LOGI("Game {} exited", state.active_package);
    state.active_package.clear();
    state.pid_tracker.invalidate();
    state.in_game_session  = false;
    state.focus_loss_count = 0;
}

/**
 * @brief Run one policy evaluation and act on the result.
 *
 * The decision itself is made by the Flux V2 Decision Engine (via FluxDecisionService), which
 * is pure and covered by host tests. Everything here is the side effects: applying the profile,
 * driving zen, and recording what happened and why.
 */
static void evaluate_and_apply(DaemonState &state, int64_t now_ms) {
    using flux::telemetry::RawSnapshot;
    using flux::telemetry::RuntimeSnapshotAssembler;

    // Age the published health first: a dead producer emits no events, so only this tick can
    // move telemetry Healthy -> Stale -> Unavailable.
    telemetry_runtime->tick();

    const auto published = telemetry_runtime->get();

    // Absent telemetry is modelled as "nothing published", never as zeroed fields. A default
    // RawSnapshot paired with Unavailable normalizes to DataHealth::Offline, which the engine
    // already treats as "fall back to a safe profile".
    const std::optional<RawSnapshot> snapshot =
        published ? std::optional<RawSnapshot>(published->snapshot) : std::nullopt;
    const flux::telemetry::TelemetryHealth health =
        published ? published->health : flux::telemetry::TelemetryHealth::Unavailable;
    const bool telemetry_healthy = (health == flux::telemetry::TelemetryHealth::Healthy);

    // A healthy snapshot proves SynthesisCore came back.
    if (telemetry_healthy && synthesiscore_down.exchange(false, std::memory_order_relaxed)) {
        LOGI("SynthesisCore telemetry restored (sequence {})", snapshot ? snapshot->sequence : 0);
        set_module_description_status("\xF0\x9F\x98\x8B Tweaks applied successfully");
    }

    // --- Game session bookkeeping ------------------------------------------
    // Only trust a healthy snapshot to *start* a session. A stale one may name a game that
    // exited minutes ago.
    if (snapshot && telemetry_healthy) {
        if (state.in_game_session && !state.active_package.empty()) {
            if (!is_game_still_active(state, *snapshot)) [[unlikely]] {
                handle_game_exit(state);
            }
        }

        if (state.active_package.empty()) {
            state.active_package = get_active_game(*snapshot, game_registry);
            if (!state.active_package.empty()) {
                state.in_game_session = true;
                LOGI("Game session started: {}", state.active_package);
            }
        }
    }

    // A game whose process died is not a game session, regardless of telemetry health.
    if (state.in_game_session && state.pid_tracker.get_current_pid() == 0) {
        handle_game_exit(state);
    }

    // --- Decide -------------------------------------------------------------
    const auto *game_entry =
        state.active_package.empty() ? nullptr : game_registry.find_game_ptr(state.active_package);

    // A game that vanished from the registry (user removed it in the WebUI) ends the session.
    if (state.in_game_session && !state.active_package.empty() && !game_entry) {
        LOGI("Game {} no longer in registry, ending session", state.active_package);
        handle_game_exit(state);
    }

    // Normalize once, through the assembler. Main.cpp never interprets telemetry itself: it does
    // not read thermal headroom, decide profiles, or invent values for absent providers. The
    // assembler resolves the foreground provider by priority (native ProcessEventSource when it
    // exists -> SynthesisCore fallback -> unavailable); nullopt below means no native provider is
    // implemented yet.
    const RawSnapshot raw = snapshot ? *snapshot : RawSnapshot{};
    const RuntimeSnapshotAssembler assembler;
    const auto assembled =
        assembler.assemble(raw, health, RuntimeSnapshotAssembler::select_foreground(std::nullopt, raw));

    flux::engine::DecisionInputs inputs;
    inputs.runtime      = assembled.runtime;
    inputs.capabilities = assembled.capabilities;
    inputs.session.in_session = state.in_game_session;
    inputs.session.package    = state.active_package;
    inputs.session.forces_lite =
        game_entry ? (game_entry->lite_mode || config_store.get_preferences().enforce_lite_mode) : false;
    inputs.shutdown_requested = daemon_stop_requested.load(std::memory_order_relaxed);

    const FluxProfileMode previous = state.policy_state.current;
    const PolicyDecision decision  = state.decision_service.decide(inputs, state.policy_state, now_ms);

    // The full V2 decision behind the adapter's summary. The execution runtime consumes this
    // rather than the FluxProfileMode: the mode has already thrown away the reason, the
    // priority and the safety constraints, and the intent mapper needs all three to know
    // whether a Balanced ask is a preference or a thermal response.
    const flux::engine::Decision &engine_decision = state.decision_service.last_decision();

    // --- Act ----------------------------------------------------------------
    const bool in_perf_tier =
        (decision.profile == PERFORMANCE_PROFILE || decision.profile == PERFORMANCE_LITE_PROFILE);

    pid_t game_pid = 0;
    if (in_perf_tier && !state.active_package.empty()) {
        game_pid = pidof_game(state.active_package, snapshot);

        // For Moonton/MLBB titles the process that actually renders is a child thread group.
        const pid_t mlbb_pid = pidof(state.active_package + ":UnityKillsMe", true);
        const pid_t tracked  = (mlbb_pid != 0) ? mlbb_pid : game_pid;
        if (tracked != 0) state.pid_tracker.set_pid(tracked);

        if (game_pid == 0) {
            LOGW("Cannot resolve PID for {}, ending session", state.active_package);
            handle_game_exit(state);
            return; // re-evaluated on the next tick with the session cleared
        }
    }

    // --- Detect drift -------------------------------------------------------
    // Something outside Flux may have moved a node Flux verified: another module, a vendor
    // service, a user with a terminal. The engine's idempotency cache would otherwise skip the
    // rewrite forever, because it still believes the value is in place. Re-reading only what
    // Flux actually verified keeps this to a handful of reads per tick.
    if (const auto drifted = execution_runtime->poll_external_mutation(now_ms); !drifted.empty()) {
        LOGW("{} capability(ies) changed outside Flux; re-applying", drifted.size());
    }

    // --- Apply --------------------------------------------------------------
    // The single live apply path. Everything from here — intent mapping, capability probing,
    // planning, validation, writing, verification, rollback and zen — happens inside the
    // execution runtime. Main.cpp hands it a decision and a session, and reads back what
    // actually happened. It does not write, and it does not interpret.
    //
    // Note this runs even when !decision.changed: the runtime does its own coalescing, and it
    // is the only thing that knows whether the device still holds what was last verified. An
    // unchanged decision against a device that drifted is real work; skipping it here — as the
    // legacy path did — is how a profile silently stopped being applied.
    flux::execution::SessionContext session;
    session.in_session = state.in_game_session;
    session.package    = state.active_package;
    session.pid        = static_cast<int>(game_pid);
    session.uid        = (snapshot && snapshot->focused_package == state.active_package)
                             ? snapshot->focused_uid
                             : 0;
    session.wants_zen  = in_perf_tier && game_entry && game_entry->enable_dnd;
    session.desired_zen_mode = ZEN_MODE_IMPORTANT_INTERRUPTIONS;

    const auto cycle = execution_runtime->on_decision(engine_decision, session, now_ms);
    if (cycle.coalesced) return;

    const auto &runtime_state = execution_runtime->state();
    const bool applied = cycle.apply.verified_active;

    if (!applied) {
        // Do not claim a profile is active when applying it did not verify. Roll the recorded
        // policy state back so the next evaluation retries rather than believing the job is done.
        LOGE("Profile {} not verified ({}): {}", profile_mode_string(decision.profile),
             flux::execution::apply_state_name(runtime_state.state()), cycle.apply.message);
        state.policy_state.current = previous;
    } else if (decision.changed) {
        LOGI("Profile {} -> {} ({}) [{}]", profile_mode_string(previous),
             profile_mode_string(decision.profile), transition_reason_string(decision.reason),
             flux::execution::apply_state_name(runtime_state.state()));
    }

    if (!decision.changed && applied) return; // nothing transitioned; nothing to record

    // --- Record -------------------------------------------------------------
    TransitionRecord record;
    record.from         = previous;
    record.to           = applied ? decision.profile : previous;
    record.reason       = decision.reason;
    record.monotonic_ms = now_ms;
    record.package      = state.active_package;
    record.health       = health;
    record.applied      = applied;
    record.apply_error  = applied ? std::string() : cycle.apply.message;
    if (snapshot && snapshot->has_thermal()) {
        record.thermal_headroom = snapshot->thermal_headroom;
        record.thermal_valid    = true;
    }
    state.history.record(record);
}

// ---------------------------------------------------------------------------
// Main daemon loop
// ---------------------------------------------------------------------------

static void flux_main_daemon() {
    DaemonState state;
    pthread_setname_np(pthread_self(), "MainThread");

    // Wake the loop the moment a tracked process dies.
    state.pid_tracker.set_callback([](pid_t) { signal_daemon_update(); });

    // The loop is woken by:
    //   - inotify telemetry updates (via signal_daemon_update)
    //   - PID tracker process-death callbacks
    //   - signal_daemon_stop()
    //   - and, crucially, a periodic timeout.
    //
    // The timeout is what makes telemetry staleness detectable at all. If SynthesisCore dies,
    // it stops writing, so no further inotify events arrive — a purely event-driven loop
    // would block in poll() indefinitely and never notice. The old loop used poll(..., -1)
    // and had exactly that hole; it only avoided sleeping forever because the death of the
    // Java lock killed the whole daemon.
    struct pollfd pfd = {synthesis_core_event_fd, POLLIN, 0};

    while (!daemon_stop_requested.load(std::memory_order_relaxed)) {
        const int ret = poll(&pfd, 1, DAEMON_TICK_MS);

        if (ret < 0) {
            if (errno == EINTR) continue;
            LOGE_TAG("MainThread", "poll() failed: {}", strerror(errno));
            break;
        }

        if (daemon_stop_requested.load(std::memory_order_relaxed)) [[unlikely]]
            break;

        if (ret > 0 && (pfd.revents & POLLIN)) {
            // Drain all coalesced wakeups in one read.
            uint64_t val = 0;
            const ssize_t rd = read(synthesis_core_event_fd, &val, sizeof(val));
            (void)rd;
        }

        // Runs on every wakeup *and* on every timeout — the timeout path is what drives the
        // Healthy -> Stale -> Offline transitions when the producer has gone quiet.
        evaluate_and_apply(state, flux_monotonic_ms());
    }

    // Put the device back exactly as we found it — values and zen — whatever happens. This is a
    // policy decision, so it is made here and not inside the runtime's shutdown().
    if (execution_runtime) {
        const auto restored = execution_runtime->restore_all("daemon_stopping", flux_monotonic_ms());
        if (!restored.verified_active) {
            LOGW("Could not fully restore original values on exit: {}", restored.message);
        }
    }
}

// ---------------------------------------------------------------------------
// Daemon bootstrap
// ---------------------------------------------------------------------------

// Will be called by FluxCLI
int run_daemon() {
    std::atexit([]() {
        SignalHandler::cleanup_before_exit();
    });

    SignalHandler::setup_signal_handlers();

    if (daemon_lock.is_locked()) {
        std::cerr << "\033[31mERROR:\033[0m Another instance of Flux Daemon is already running!\n";
        return EXIT_FAILURE;
    }

    if (access(MODULE_UPDATE, F_OK) == 0) {
        notify("Please reboot your device to complete module update.");
        return EXIT_FAILURE;
    }

    if (access(FLUX_GAMELIST, F_OK) != 0) {
        std::cerr << "\033[31mERROR:\033[0m " << FLUX_GAMELIST << " is missing\n";
        notify_fatal_error("gamelist.json is missing");
        LOGC("{} is missing", FLUX_GAMELIST);
        return EXIT_FAILURE;
    }

    if (!game_registry.load_from_json(FLUX_GAMELIST)) {
        std::cerr << "\033[31mERROR:\033[0m Failed to parse " << FLUX_GAMELIST << '\n';
        notify_fatal_error("Failed to parse gamelist.json");
        LOGC("Failed to parse {}", FLUX_GAMELIST);
        return EXIT_FAILURE;
    }

    if (!device_mitigation_store.load_config()) {
        std::cerr << "\033[31mERROR:\033[0m Failed to parse " << DEVICE_MITIGATION_FILE << '\n';
        notify_fatal_error("Failed to parse device_mitigation.json");
        LOGC("Failed to parse {}", DEVICE_MITIGATION_FILE);
        return EXIT_FAILURE;
    }

    if (daemon(0, 0) != 0) {
        LOGC("Failed to daemonize service");
        notify_fatal_error("Failed to daemonize service");
        return EXIT_FAILURE;
    }

    if (!create_lock_file()) {
        LOGC("Failed acquire daemon lock after daemonize");
        notify_fatal_error("Failed to acquire lock");
        return EXIT_FAILURE;
    }

    // eventfd for immediate daemon wake-up on synthesis_core changes and PID
    // tracker callbacks.
    synthesis_core_event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (synthesis_core_event_fd < 0) {
        LOGC("Failed to create eventfd: {}", strerror(errno));
        notify_fatal_error("Failed to create eventfd");
        return EXIT_FAILURE;
    }

    // Bring up the single live telemetry authority. The watcher registers its directory watch
    // before the first read, so a snapshot written during startup is either seen by that read or
    // still queued for the worker — it cannot fall through the gap between the two.
    telemetry_runtime = std::make_unique<flux::telemetry::TelemetryRuntime>(
        CONFIG_DIR, "synthesis_core.json", [] { return flux_monotonic_ms(); },
        [] { signal_daemon_update(); }
    );
    if (!telemetry_runtime->start()) {
        LOGC("Failed to start telemetry watcher: {}", telemetry_runtime->last_error());
        notify_fatal_error("Failed to start telemetry watcher");
        close(synthesis_core_event_fd);
        synthesis_core_event_fd = -1;
        return EXIT_FAILURE;
    }

    // --- The live execution runtime ----------------------------------------
    // Constructed after telemetry, because the zen backend reads the live mode through it, and
    // destroyed before it. This is the process's only ExecutionEngine and only NodeBackend.
    {
        zen_backend = std::make_unique<flux::execution::AndroidZenBackend>(
            []() -> std::optional<int> {
                // The user's current zen mode, from the one telemetry authority. Never a
                // subprocess: SynthesisCore already publishes this, and asking the system a
                // second time would be a second source of truth for one value.
                const auto published = telemetry_runtime->get();
                if (!published || !published->snapshot.zen_available) return std::nullopt;
                return published->snapshot.zen_mode;
            },
            [](int mode) {
                set_zen_mode(mode);
                return true;
            });

        // The SoC selects which packs apply. Every vendor pack ships gated: being selected does
        // not make it executable, and nothing here promotes it.
        const flux::execution::SocFamily soc = read_soc_family();
        const flux::execution::DeviceIdentity identity{soc, DeviceInfo::get_soc_model()};

        // packs_for() already puts the generic pack first: it is the fallback every device
        // gets, and the vendor pack (if any) is added on top of it.
        auto packs = flux::device::packs_for(soc);

        execution_runtime = std::make_unique<flux::execution::ExecutionRuntime>(
            std::move(packs), identity, flux::execution::PathPolicy{}, zen_backend.get());

        // Publish the verified state for the WebUI and the CLI. The profile written here is the
        // *verified* one — what the device is actually in — not what policy asked for.
        execution_runtime->set_status_publisher(
            [](const flux::execution::RuntimeProfileState &runtime_state,
               const flux::execution::SessionContext &session) {
                write2file(PROFILE_MODE,
                           static_cast<int>(profile_mode_from_target(
                               runtime_state.verified_profile(), runtime_state.has_verified_profile())),
                           "\n");
                if (session.in_session && !session.package.empty()) {
                    write2file(GAME_INFO, session.package, " ", session.pid, " ", session.uid, "\n");
                } else {
                    write2file(GAME_INFO, "NULL 0 0\n");
                }
            });

        // Migrate stored configuration before the first cycle, so the very first apply already
        // honours the user's settings rather than applying defaults and correcting itself.
        install_tuning_from_config(flux_monotonic_ms());

        LOGI("Execution runtime up: soc={} tweaks={} (vendor capabilities gated pending "
             "hardware validation)",
             flux::execution::soc_family_name(soc),
             execution_runtime->tweaks_enabled() ? "enabled" : "disabled by configuration");
    }

    // Give SynthesisCore a grace period to come up, but do not make Flux's existence
    // conditional on it.
    //
    // This used to exit the daemon outright if the producer had not appeared within 120 s.
    // Combined with the watch below (which also used to exit on producer death), Flux could
    // not run at all without SynthesisCore — a telemetry outage meant no policy engine, and
    // therefore no thermal protection, rather than a degraded one. Flux now starts, applies a
    // safe profile, and picks telemetry up whenever it arrives.
    {
        int waited = 0;
        constexpr int GRACE_SECONDS = 30;
        while (!java_lock.is_locked() && waited < GRACE_SECONDS) {
            if (waited == 0) LOGI("Waiting for SynthesisCore to start...");
            sleep(1);
            ++waited;
        }

        if (!java_lock.is_locked()) {
            synthesiscore_down.store(true, std::memory_order_relaxed);
            LOGW("SynthesisCore did not start within {}s. Continuing in safe mode; "
                 "telemetry will be picked up if it appears later.", GRACE_SECONDS);
            set_module_description_status("\xE2\x9A\xA0 Telemetry unavailable, running safe profile");
        } else {
            LOGI("SynthesisCore is up");
        }
    }

    // Watch SynthesisCore's liveness. Its death degrades Flux; it does not stop it.
    watch_java_lock();

    InotifyWatcher file_watcher;
    if (!init_file_watcher(file_watcher)) {
        LOGC("Failed to initialize file watcher");
        notify_fatal_error("Failed to initialize file watcher");
        close(synthesis_core_event_fd);
        synthesis_core_event_fd = -1;
        return EXIT_FAILURE;
    }

    LOGI("Flux daemon started");
    set_module_description_status("\xF0\x9F\x98\x8B Tweaks applied successfully");
    flux_main_daemon();

    LOGW("Flux daemon exited");

    // Stop the watcher and join its worker before the wake descriptor it signals is closed.
    if (telemetry_runtime) {
        telemetry_runtime->stop();
    }

    if (synthesis_core_event_fd >= 0) {
        close(synthesis_core_event_fd);
        synthesis_core_event_fd = -1;
    }

    SignalHandler::cleanup_before_exit();
    return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    if (getuid() != 0) {
        std::cerr << "\033[31mERROR:\033[0m Please run this program as root\n";
        return EXIT_FAILURE;
    }
    return flux_cli(argc, argv);
}
