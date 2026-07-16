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

#include "ZenController.hpp"

namespace flux::execution {

bool ZenController::engage(int desired_mode) {
    if (!backend_.available()) return false;

    if (!engaged_) {
        // Capture the exact prior mode once, so release restores it precisely. If the current
        // mode cannot be read we refuse to engage rather than restore to a guessed value.
        const std::optional<int> current = backend_.read();
        if (!current) return false;
        original_ = current;
    }

    if (!backend_.set(desired_mode)) {
        // A failed set leaves us un-engaged on the first attempt (nothing to restore); a failed
        // re-apply keeps the recorded original intact for a later restore.
        if (original_ && !engaged_) original_.reset();
        return false;
    }

    applied_mode_ = desired_mode;
    engaged_ = true;
    return true;
}

bool ZenController::restore() {
    if (!engaged_) return true; // nothing engaged, nothing to undo
    if (!backend_.available()) return false;
    if (!original_) return false; // defensive: engaged without a recorded original

    // Respect a concurrent user change: if the live mode is no longer what Flux applied, the
    // user (or another controller) moved it deliberately. Do not overwrite that choice.
    const std::optional<int> current = backend_.read();
    if (current && *current != applied_mode_) {
        engaged_ = false;
        original_.reset();
        return false;
    }

    if (!backend_.set(*original_)) return false;

    engaged_ = false;
    original_.reset();
    return true;
}

} // namespace flux::execution
