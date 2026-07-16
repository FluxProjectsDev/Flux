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

#include "InotifyHandler.hpp"

#include "DeviceMitigationStore.hpp"
#include "FluxConfigStore.hpp"

#include <Flux.hpp>
#include <FluxLog.hpp>
#include <FluxUtility.hpp>
#include <GameRegistry.hpp>
#include <InotifyEvents.hpp>

// signal_daemon_update and signal_daemon_stop are defined in Main.cpp
extern void signal_daemon_update();
extern void signal_daemon_stop();

enum WatchContext {
    WATCH_CONTEXT_GAMELIST,
    WATCH_CONTEXT_CONFIG,
    WATCH_CONTEXT_DEVICE_MITIGATION,
    WATCH_CONTEXT_MODULE_UPDATE,
};

void on_json_modified(const struct inotify_event *event, const std::string &path, int context, void *additional_data) {
    (void)additional_data;

    auto OnGamelistModified = [&](const std::string &path) -> void {
        LOGD_TAG("InotifyHandler", "Callback OnGamelistModified reached");
        game_registry.load_from_json(path);
    };

    auto OnDeviceMitigationModified = [&](const std::string &path) -> void {
        LOGD_TAG("InotifyHandler", "Callback OnDeviceMitigationModified reached");
        device_mitigation_store.load_config(path);
    };

    auto OnConfigModified = [&](const std::string &path) -> void {
        LOGD_TAG("InotifyHandler", "Callback OnConfigModified reached");

        if (!config_store.reload()) {
            LOGW_TAG("InotifyHandler", "Failed to reload config from {}", path);
            return;
        }

        // Apply new log level
        auto prefs = config_store.get_preferences();
        FluxLog::set_log_level(prefs.log_level);
    };

    auto OnModuleUpdateCreated = [&]() -> void {
        LOGI_TAG("InotifyHandler", "Module update file detected, signaling daemon to stop");
        notify("Please reboot your device to complete module update.");
        signal_daemon_stop();
    };

    // The kernel dropped events. We cannot know what we missed, so re-read everything rather
    // than assume our view is current. Telemetry is not re-read here: AtomicStatusWatcher owns
    // the status file and handles IN_Q_OVERFLOW itself, flagging the resync as uncertain.
    if (is_overflow(event->mask)) {
        LOGW_TAG("InotifyHandler", "inotify queue overflowed, re-reading watched files");
        OnGamelistModified(FLUX_GAMELIST);
        OnConfigModified(CONFIG_FILE);
        OnDeviceMitigationModified(DEVICE_MITIGATION_FILE);
        return;
    }

    // React to *any* event that means the file has new content.
    //
    // is_content_update() accepts IN_CLOSE_WRITE and IN_MOVED_TO. The second one is the
    // important one and was previously missing: SynthesisCore replaces the status file with
    // rename(), and a rename never delivers IN_CLOSE_WRITE on the destination name — only
    // IN_MOVED_TO. Gating on IN_CLOSE_WRITE alone meant the telemetry cache was seeded once
    // at startup and then never updated again for the entire uptime of the device.
    if (is_content_update(event->mask)) {
        switch (context) {
            case WATCH_CONTEXT_GAMELIST: OnGamelistModified(path); break;
            case WATCH_CONTEXT_CONFIG: OnConfigModified(path); break;
            case WATCH_CONTEXT_DEVICE_MITIGATION: OnDeviceMitigationModified(path); break;
            default: break;
        }
    }

    // React when MODULE_UPDATE is created inside MODPATH directory
    if ((event->mask & IN_CREATE) && context == WATCH_CONTEXT_MODULE_UPDATE) {
        if (event->len > 0 && std::string(event->name) == "update") {
            OnModuleUpdateCreated();
        }
    }
}

bool init_file_watcher(InotifyWatcher &watcher) {
    try {
        // Initialize config store first
        if (!config_store.load_config()) {
            LOGE_TAG("FluxConfig", "Failed to load config file");
            return false;
        }

        // Apply log level from config
        auto prefs = config_store.get_preferences();
        FluxLog::set_log_level(prefs.log_level);

        // Set up file watchers
        InotifyWatcher::WatchReference gamelist_ref{FLUX_GAMELIST, on_json_modified, WATCH_CONTEXT_GAMELIST, nullptr};

        InotifyWatcher::WatchReference config_ref{CONFIG_FILE, on_json_modified, WATCH_CONTEXT_CONFIG, nullptr};

        InotifyWatcher::WatchReference device_mitigation_ref{
            DEVICE_MITIGATION_FILE, on_json_modified, WATCH_CONTEXT_DEVICE_MITIGATION, nullptr
        };

        // Watch the module directory for creation of the "update" file.
        // The file does not exist yet, so we must watch the parent directory.
        InotifyWatcher::WatchReference module_update_ref{MODPATH, on_json_modified, WATCH_CONTEXT_MODULE_UPDATE, nullptr};

        if (!watcher.addFile(gamelist_ref)) {
            LOGE_TAG("InotifyWatcher", "Failed to add gamelist watch");
            return false;
        }

        if (!watcher.addFile(config_ref)) {
            LOGE_TAG("InotifyWatcher", "Failed to add config watch");
            return false;
        }

        if (!watcher.addFile(device_mitigation_ref)) {
            LOGE_TAG("InotifyWatcher", "Failed to add device mitigation watch");
            return false;
        }

        if (!watcher.addDirectory(module_update_ref)) {
            LOGE_TAG("InotifyWatcher", "Failed to add module update watch");
            return false;
        }

        // Telemetry is deliberately NOT seeded here any more. AtomicStatusWatcher owns the
        // status file: it registers its own directory watch before its first read, so the
        // startup race this seeding used to close is now handled inside the telemetry runtime.
        // Seeding here as well would create a second telemetry path.

        if (!watcher.start()) {
            LOGE_TAG("InotifyWatcher", "Failed to start watcher thread");
            return false;
        }
        return true;
    } catch (const std::runtime_error &e) {
        std::string error_msg = e.what();
        LOGE_TAG("InotifyWatcher", "{}", error_msg);
        return false;
    }
}
