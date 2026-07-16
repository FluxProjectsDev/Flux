# NOTICE

Flux Tweaks
Copyright (C) 2024-2026 FebriCahyaa

This product includes software developed as part of the Flux project and
software derived from third-party works, as described below. This NOTICE file
is provided in accordance with Section 4(d) of the Apache License, Version 2.0.

Operational branding (release destinations, artifact names, user-facing product
names, notification text) is Flux-owned. The attributions below are retained for
legal reasons and MUST NOT be removed merely to eliminate upstream references.
Each attribution is removed only after the corresponding component has been
independently reimplemented and validated, and no derived code, assets,
structure, or distinctive expression from that source remains in the distributed
component. See `docs/provenance-matrix.md` for the per-component status.

--------------------------------------------------------------------------------

## 1. Derived work — Encore Tweaks

Portions of Flux are materially derived from:

    Encore Tweaks
    Copyright (C) 2024-2026 Rem01Gaming
    Licensed under the Apache License, Version 2.0
    https://github.com/Rem01Gaming/encore

The following areas of the current Flux distribution contain code, scripts,
data, module structure, or WebUI expression that is adapted from Encore Tweaks
and remains under Apache-2.0 while that derived material is present:

  - the native daemon and supporting modules under `jni/` (excluding the Flux V2
    engine under `jni/engine/` and the host tests under `jni/tests/`, which are
    Flux-authored);
  - the profiler and utility shell scripts under `scripts/`;
  - the Magisk/KernelSU/APatch module scripts under `module/`;
  - the WebUI application under `webui/src/`;
  - the game-detection data in `gamelist.txt`;
  - the per-SoC device tuning knowledge expressed as declarative capability
    descriptors in `jni/device/` (re-expressed as data and gated behind runtime
    capability probing; the execution mechanism that consumes them is
    Flux-authored — see `jni/engine/execution/`, below).

Files adapted from Encore Tweaks carry both the original copyright notice and a
notice that they have been modified by the Flux project. These attributions will
be retired per component as each area is independently rewritten under the Flux
Platform V2 program.

--------------------------------------------------------------------------------

## 2. Bundled third-party components

### spdlog
Copyright (c) 2016 - present, Gabi Melman and spdlog contributors.
Licensed under the MIT License.
Included as a git submodule at `jni/external/spdlog`.
https://github.com/gabime/spdlog

### RapidJSON
Copyright (C) 2015 THL A29 Limited, a Tencent company, and Milo Yip.
Licensed under the MIT License (binary distribution).
Included as a git submodule at `jni/external/rapidjson`.
https://github.com/Tencent/rapidjson

> **Removed:** The WebUI previously bundled "Google Sans Flex" woff2 font files
> whose redistribution terms are not Apache-2.0. Those assets and their
> `@font-face` declarations have been removed; the WebUI now uses a system-first
> font stack (see `webui/src/assets/main.css`) and ships no bundled typeface.

--------------------------------------------------------------------------------

## 3. WebUI runtime dependencies

The WebUI bundles production JavaScript dependencies resolved via `webui/bun.lock`
(including Vue, Vue Router, Pinia, Tailwind CSS, kernelsu, webuix). Each carries
its own license (predominantly MIT); those licenses are preserved by their
respective packages. A full software bill of materials (SBOM) is produced as part
of the release pipeline.

--------------------------------------------------------------------------------

## 4. Flux-owned original work

The following are original Flux work and are not derived from Encore Tweaks:

  - the Flux V2 Decision Engine (`jni/engine/decision/`), including its
    compatibility adapter;
  - the Flux V2 telemetry pipeline (`jni/engine/telemetry/`): strict schema-v2
    decoder, freshness/health model, thread-safe store, and provider-neutral
    RuntimeSnapshot assembler;
  - the Flux V2 Execution Engine (`jni/engine/execution/`): capability registry,
    value/path validator, planner, and transactional verified/rollback-aware
    apply;
  - the OFD-lock process-lock implementation (`jni/base/LockFile/`);
  - SynthesisCore, the telemetry provider consumed by Flux (its integration layer
    now lives in `jni/engine/telemetry/`, above; the former
    `jni/base/SynthesisCore/` reader has been removed);
  - the daemon host-side tests (`jni/tests/`) and the SynthesisCore dependency
    tooling (`scripts/*synthesiscore*`).

See the provenance matrix for the authoritative per-component classification.
