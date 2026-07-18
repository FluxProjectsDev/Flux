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

#include <functional>
#include <optional>

#include "ZenController.hpp"

/**
 * @file AndroidZenBackend.hpp
 * @brief The production ZenBackend: the single zen write entry point.
 *
 * Flux-owned (Category A).
 *
 * Zen is the one capability that is not a device node. Android exposes it through
 * `cmd notification set_dnd`, so it cannot go through SysfsNodeBackend and has a backend of its
 * own — but it goes through the same ZenController, and therefore obeys the same rules: capture
 * the exact original mode, apply the exact requested one, restore the exact original, and never
 * overwrite a change the user made themselves.
 *
 * Reads come from telemetry, not from a subprocess. SynthesisCore already publishes the live zen
 * mode; shelling out to ask a second time would be a second source of truth for a value Flux
 * already has, and the two could disagree.
 *
 * Both dependencies are injected, so host tests exercise the production controller without
 * forking anything.
 */
namespace flux::execution {

/// Reads the current zen mode. nullopt when zen is unavailable or nothing has been published.
using ZenModeReader = std::function<std::optional<int>()>;

/// Performs the actual mode change. Returns false when it could not be applied.
using ZenModeWriter = std::function<bool(int)>;

/**
 * @brief The one production zen backend.
 *
 * `available()` is false when telemetry cannot tell us the current mode. That matters: without a
 * reliable original, engaging zen would mean Flux could not put it back, and silently leaving a
 * user's phone in Do-Not-Disturb is worse than not engaging it at all.
 */
class AndroidZenBackend : public ZenBackend {
public:
    AndroidZenBackend(ZenModeReader reader, ZenModeWriter writer)
        : reader_(std::move(reader)), writer_(std::move(writer)) {}

    [[nodiscard]] bool available() const override { return reader_ && reader_().has_value(); }

    [[nodiscard]] std::optional<int> read() const override {
        return reader_ ? reader_() : std::nullopt;
    }

    bool set(int mode) override {
        if (!writer_) return false;
        if (mode < 0 || mode > 3) return false; // the exact enum, never a coerced boolean
        return writer_(mode);
    }

private:
    ZenModeReader reader_;
    ZenModeWriter writer_;
};

} // namespace flux::execution
