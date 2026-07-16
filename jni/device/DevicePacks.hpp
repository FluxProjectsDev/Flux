/*
 * Copyright (C) 2024-2026 Rem01Gaming
 * Copyright (C) 2024-2026 FebriCahyaa
 *
 * The per-SoC tuning knowledge expressed by these packs is derived from Encore Tweaks
 * (https://github.com/Rem01Gaming/encore), by way of the per-SoC blocks of
 * scripts/flux_profiler.sh. Modified by the Flux project: re-expressed as declarative data,
 * re-scoped, and gated behind runtime capability probing.
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

#include <vector>

#include "DeviceDescriptor.hpp"

/**
 * @file DevicePacks.hpp
 * @brief Declarative device packs. **Data only** — see jni/device/README.md.
 *
 * Category C. Nothing here writes, reads, chmods, forks, or decides a profile. These functions
 * return values; the Category A engine does everything else.
 */
namespace flux::device {

/// Policy intents these packs speak. Kept as strings rather than an enum so a pack can name an
/// intent this build does not implement without failing to parse — the descriptor simply
/// produces no action for it.
inline constexpr const char *kIntentSustainedPerformance = "sustained_performance";
inline constexpr const char *kIntentConstrainedPerformance = "constrained_performance";
inline constexpr const char *kIntentBalanced = "balanced";
inline constexpr const char *kIntentSafe = "safe";

/** Every candidate pack, one per SoC family, plus the generic set. */
std::vector<flux::execution::DevicePack> all_packs();

/** The packs that could apply to @p soc: the generic set plus that family's set. */
std::vector<flux::execution::DevicePack> packs_for(flux::execution::SocFamily soc);

flux::execution::DevicePack generic_pack();
flux::execution::DevicePack mediatek_pack();
flux::execution::DevicePack snapdragon_pack();
flux::execution::DevicePack exynos_pack();
flux::execution::DevicePack unisoc_pack();
flux::execution::DevicePack tensor_pack();
flux::execution::DevicePack tegra_pack();

} // namespace flux::device
