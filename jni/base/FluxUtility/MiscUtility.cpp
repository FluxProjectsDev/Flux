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

#include "FluxUtility.hpp"

#include <chrono>

#include <ModuleProperty.hpp>
#include <ShellUtility.hpp>

const char *zen_mode_to_dnd_arg(int zen_mode) {
    // Maps Android's Settings.Global.ZEN_MODE_* onto the argument `cmd notification set_dnd`
    // expects. The two vocabularies do not share names, which is part of why this was
    // previously collapsed to a boolean.
    switch (zen_mode) {
        case ZEN_MODE_OFF: return "off";
        case ZEN_MODE_IMPORTANT_INTERRUPTIONS: return "priority";
        case ZEN_MODE_NO_INTERRUPTIONS: return "none"; // total silence
        case ZEN_MODE_ALARMS: return "alarms";
        default: return "off";
    }
}

void set_zen_mode(int zen_mode) {
    const char *mode_arg = zen_mode_to_dnd_arg(zen_mode);

    pid_t pid = fork();

    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        const char *args[] = {"cmd", "notification", "set_dnd", mode_arg, NULL};

        execvp("/system/bin/cmd", (char *const *)args);
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) [[unlikely]] {
            LOGE("Failed to set zen mode '{}' with status: {}", mode_arg, WEXITSTATUS(status));
        }
    } else {
        LOGE("fork failed: {}", strerror(errno));
    }
}

void notify(const char *message) {
    pid_t pid = fork();

    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        if (setgid(2000) != 0 || setuid(2000) != 0) {
            _exit(126);
        }

        const char *args[] = {"cmd", "notification", "post", "-t", NOTIFY_TITLE, LOG_TAG, message, NULL};

        execvp("/system/bin/cmd", (char *const *)args);
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) [[unlikely]] {
            LOGE("Push notification failed with status: {}", WEXITSTATUS(status));
        }
    } else {
        LOGE("fork failed: {}", strerror(errno));
    }
}

int64_t flux_monotonic_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
