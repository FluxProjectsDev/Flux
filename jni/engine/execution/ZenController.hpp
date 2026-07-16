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

#pragma once

#include <optional>

namespace flux::execution {

/**
 * @brief The zen/Do-Not-Disturb control surface, abstracted for host testing.
 *
 * Zen mode is a full enum (0 off, 1 priority, 2 total silence, 3 alarms only), never a
 * boolean. `available()` is false on a device where the capability could not be resolved.
 */
class ZenBackend {
public:
    virtual ~ZenBackend() = default;
    virtual bool available() const = 0;
    virtual std::optional<int> read() const = 0;
    virtual bool set(int mode) = 0;
};

/**
 * @brief Applies and restores zen mode while preserving the exact original integer.
 *
 * When Flux engages a zen mode for a session it records the exact prior mode and restores
 * exactly that on release — never collapsing it to on/off. If the user changed zen while
 * Flux held it (the live value no longer matches what Flux set), restore() leaves the user's
 * choice intact instead of overwriting it. If the capability is unavailable, nothing is
 * touched. Failures are reported, never hidden.
 */
class ZenController {
public:
    explicit ZenController(ZenBackend &backend) : backend_(backend) {}

    /**
     * @brief Engage @p desired_mode, recording the exact original the first time.
     * @return true if the mode was set (or already engaged to it); false if unavailable or
     *         the set failed.
     */
    bool engage(int desired_mode);

    /**
     * @brief Restore the exact original mode.
     * @return true if restored (or nothing to restore); false if unavailable, an external
     *         change was detected (left intact), or the set failed.
     */
    bool restore();

    [[nodiscard]] bool engaged() const { return engaged_; }
    [[nodiscard]] std::optional<int> original() const { return original_; }

private:
    ZenBackend &backend_;
    bool engaged_ = false;
    std::optional<int> original_; ///< the exact mode to restore to
    int applied_mode_ = 0;        ///< what Flux last set, for external-change detection
};

} // namespace flux::execution
