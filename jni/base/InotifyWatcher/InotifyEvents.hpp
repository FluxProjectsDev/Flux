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

#pragma once

#include <cstdint>

#include <sys/inotify.h>

/**
 * @file InotifyEvents.hpp
 * @brief Event-mask predicates shared by the watcher, the handlers and the tests.
 *
 * These live in a header of their own so the tests assert against the *same* predicate the
 * daemon uses, rather than a copy of it that can drift.
 */

/**
 * @brief True when an event means "this file now has new content to read".
 *
 * Both events matter, and which one you get depends entirely on how the producer writes:
 *
 *   - `IN_CLOSE_WRITE` — the producer opened the file, wrote it in place, and closed it.
 *   - `IN_MOVED_TO`    — the producer wrote a temp file and `rename()`d it over the target.
 *
 * SynthesisCore does the latter, and has since it adopted atomic replacement. A rename
 * **never** produces `IN_CLOSE_WRITE` on the destination name — nothing ever opens the
 * destination for writing; it only appears as the target of a rename. The `IN_CLOSE_WRITE`
 * that *is* emitted carries the name of the *temp* file, which does not match the watched
 * filename and is therefore correctly ignored.
 *
 * Watching only for `IN_CLOSE_WRITE` against an atomically-replaced file means seeing the
 * file update exactly zero times. That was the live defect: Flux's telemetry cache was
 * frozen at whatever it read once at startup, so foreground app, thermal, screen and
 * battery-saver state never advanced for the entire uptime of the device.
 */
[[nodiscard]] inline bool is_content_update(uint32_t mask) {
    return (mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) != 0;
}

/**
 * @brief True when the watched file went away.
 *
 * `IN_MOVED_FROM` covers the temp file being renamed away, and `IN_DELETE` a real removal —
 * for instance the producer being uninstalled, or its config directory being wiped.
 */
[[nodiscard]] inline bool is_removal(uint32_t mask) {
    return (mask & (IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF)) != 0;
}

/**
 * @brief True when the kernel dropped events because our queue overflowed.
 *
 * The correct response is to re-read the watched files rather than to trust that we saw
 * every change, because by definition we did not.
 */
[[nodiscard]] inline bool is_overflow(uint32_t mask) {
    return (mask & IN_Q_OVERFLOW) != 0;
}
