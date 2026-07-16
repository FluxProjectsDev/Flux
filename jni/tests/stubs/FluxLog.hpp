/*
 * Copyright (C) 2024-2026 FebriCahyaa
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

/**
 * @file stubs/FluxLog.hpp
 * @brief No-op stand-in for the real FluxLog in host tests.
 *
 * The production logger is built on spdlog, which the module vendors as a git submodule and
 * links for Android. The host tests exist to exercise the *logic* — the inotify event masks,
 * the parser, the policy — and dragging spdlog into that build would add a dependency (and a
 * submodule checkout) for no coverage whatsoever.
 *
 * This stub is placed ahead of jni/include on the test include path, so `#include <FluxLog.hpp>`
 * inside the production sources resolves here and nowhere else. The production build is
 * untouched.
 */

#include <cstdint>

#define LOGT(...) ((void)0)
#define LOGD(...) ((void)0)
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
#define LOGC(...) ((void)0)

#define LOGT_TAG(...) ((void)0)
#define LOGD_TAG(...) ((void)0)
#define LOGI_TAG(...) ((void)0)
#define LOGW_TAG(...) ((void)0)
#define LOGE_TAG(...) ((void)0)
#define LOGC_TAG(...) ((void)0)

namespace FluxLog {
inline void set_log_level(int) {}
} // namespace FluxLog
