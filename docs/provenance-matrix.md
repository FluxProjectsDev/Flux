# Flux Provenance Matrix

Internal engineering and compliance record for the Flux Platform V2 rearchitecture.

**Purpose.** This document records, per component, where the current Flux
distribution stands relative to its origins so that license obligations are met
while derived material remains, and so that attribution is retired only after a
component is genuinely reimplemented. It is the authoritative source referenced
by [`NOTICE.md`](../NOTICE.md).

**What this file is not.** It does not reproduce third-party source. It records
provenance *conclusions*, not comparison dumps or copied excerpts.

## Classification legend

| Category | Meaning | Default action |
| --- | --- | --- |
| **A** | Independently written; not materially derived from Encore | Retain; test; document ownership |
| **B** | Generic/commonplace infrastructure | Evaluate normally; do not rewrite for artificial difference |
| **C** | Materially derived from / adapted from Encore Tweaks | Keep attribution; replace with independent implementation; remove legacy path after validation |
| **D** | Uncertain provenance | Treat conservatively as derived; preserve any header; resolve or replace before stable release |
| **E** | Third-party dependency consumed intentionally | Preserve license; pin version; do not modify vendored code |

Upstream reference for Category C/D: Encore Tweaks, Copyright (C) 2024–2026
Rem01Gaming, Apache-2.0 — https://github.com/Rem01Gaming/encore

## Component matrix

| Path | Purpose | Category | Evidence | Notices retained | Replacement status | Release eligibility |
| --- | --- | --- | --- | --- | --- | --- |
| `jni/Main.cpp`, `jni/Profiler.*`, `jni/InotifyHandler.*`, `jni/FluxCLI.*`, `jni/FluxConfigStore.*`, `jni/DeviceMitigationStore.*` | Native daemon entry, profiler loop, inotify handling, CLI, config/mitigation stores | C | Structure, control flow, and identifiers trace to the Encore daemon | Dual header (Rem01Gaming + FebriCahyaa + "Adapted from Encore Tweaks / Modified by the Flux project"); NOTICE §1 | **Pending** — to be replaced by the V2 engine (`telemetry`/`decision`/`execution`/`supervision`) | Ships under Apache-2.0 with attribution; not yet independent |
| `jni/base/DeviceInfo/*`, `jni/base/GameRegistry/*`, `jni/base/InotifyWatcher/*`, `jni/base/PIDTracker/*`, `jni/base/FluxUtility/*` | Device probing, game registry, inotify watcher, PID tracking, shell/process utilities | C | Adapted from corresponding Encore modules | Dual header; NOTICE §1 | **Pending** | Ships under Apache-2.0 with attribution |
| `jni/base/ProfilePolicy/ProfilePolicy.hpp` | Daemon record/diagnostic vocabulary (`PolicyState`, `PolicyDecision`, `TransitionRecord`) + enum-to-string helpers | C | Adapted from Encore | Dual header; NOTICE §1 | **Encore-derived policy logic removed.** `ProfilePolicy::evaluate`, `PolicyInputs` and `ThermalThresholds` were deleted in the Stage 1 telemetry cutover; `ProfilePolicy.cpp` no longer exists and no `ProfilePolicy` library is built. What remains is a header-only vocabulary the daemon's record path still speaks, retired in the Diagnostics-ownership phase | Ships with attribution until the remaining vocabulary is retired |
| `jni/engine/decision/DecisionEngine.{hpp,cpp}` | **Flux V2 Decision Engine** — deterministic, side-effect-free policy | A | Flux-authored from the Flux spec, RuntimeSnapshot semantics, and SynthesisCore v2; own vocabulary (TargetProfile/DecisionReason/DecisionPriority); 29 host tests | Flux copyright only; NOTICE §4 | N/A — this is the replacement | Eligible |
| `jni/engine/decision/DecisionAdapter.{hpp,cpp}` | Short-lived compat boundary (daemon record vocabulary ⇄ V2 engine) | A | Flux-authored; documented purpose + removal condition; covered by behavioural vectors | Flux copyright only | Narrowed in Stage 1: the telemetry conversion helpers (`build_runtime_snapshot`, `build_capabilities`) and the legacy `decide(PolicyInputs, …)` overload are deleted — only profile/reason enum mapping remains. Remove entirely when daemon record/diagnostics types are V2-native | Eligible |
| `jni/engine/telemetry/*` | **Flux V2 telemetry pipeline** — strict decoder, freshness/health, thread-safe store, provider-neutral RuntimeSnapshot assembler, live `TelemetryRuntime` | A | Flux-authored from the SynthesisCore v2 contract; was never a translation of the removed `jni/base/SynthesisCore`; 35 + 9 host tests | Flux copyright only; NOTICE §4 | **Done** — this is the sole production telemetry implementation. The legacy reader is deleted, and its absence from `fluxd` is proven per ABI by `.github/scripts/verify-native-telemetry.sh` | Eligible |
| `jni/device/*` | **Declarative device packs** — per-SoC capability descriptors (data only) | C | Device knowledge (paths, values) derived from Encore Tweaks via the per-SoC blocks of `scripts/flux_profiler.sh`; re-expressed as declarative data, not translated control flow | Dual header (Rem01Gaming + FebriCahyaa + "derived from Encore Tweaks / Modified by the Flux project"); NOTICE §1; per-pack `attribution` field enforced by `DescriptorValidator` | **Retained with attribution.** Isolated from the Category A engine so the mechanism stays independent. Attribution may be retired **per pack**, only after that family's table is independently re-derived and validated on hardware | Ships under Apache-2.0 with attribution; every descriptor is inert (`PhysicalDeviceRequired`) until validated on real hardware |
| `jni/engine/execution/*` | **Flux V2 Execution Engine** — capability registry, value/path validator, planner, transactional apply with verify/rollback/idempotency, restoration, bounded apply history | A | Flux-authored from Linux sysfs semantics + Flux safety requirements; injectable node backend; 16 host tests | Flux copyright only; NOTICE §4 | Supersedes the legacy profile applier once the runtime is migrated onto it (migration in progress) | Eligible |
| `jni/include/*.hpp` (`Flux.hpp`, `FluxLog.hpp`, `ModuleProperty.hpp`, `ShellUtility.hpp`, `SignalHandler.hpp`, `Write2File.hpp`) | Shared daemon headers/utilities | C | Adapted from Encore headers | Dual header; NOTICE §1 | Pending | Ships under Apache-2.0 with attribution |
| ~~`jni/base/SynthesisCore/*`~~ (removed) | Legacy SynthesisCore telemetry integration (JSON cache reader) | A — **retired** | Was Flux-authored, but superseded by `jni/engine/telemetry/`. Keeping two telemetry readers compiled meant two sources of truth and a fallback that could silently re-take the runtime | n/a (removed) | **Done** — deleted in the Stage 1 telemetry cutover along with `SynthesisCoreReader` and the `synthesis_core_cache`. Its absence from the production build is proven by object-manifest, archive and binary-symbol checks, not by grep | n/a (not built, not shipped) |
| `jni/base/LockFile/*` | Single-instance / duplicate-daemon lock | A | **Resolved:** reimplemented independently on Linux OFD locks (`fcntl(F_OFD_SETLK)`); Apache-2.0/Flux header; 12 host tests | Flux copyright only; NOTICE §4 | Replaced (was Category D) | Eligible |
| `jni/tests/*` (`TelemetryPipelineTest`, `TelemetryRuntimeTest`, `DecisionEngineTest`, `DecisionVectorsTest`, `ExecutionEngineTest`, `ZenControllerTest`, `RuntimeIntegrationTest`, `InotifyIntegrationTest`, `TestFramework.hpp`, `run_tests.sh`, stubs) | Host-side unit/integration tests | A | Flux-authored; Flux-only header; no Encore counterpart | Flux copyright only; NOTICE §4 | The Encore-derived `ProfilePolicyTest`/`TelemetryParserTest` and the `DecisionParityTest` harness were removed with the implementations they tested; `DecisionVectorsTest` replaces the parity harness with golden behavioural vectors that no longer execute legacy code | Eligible |
| `scripts/flux_profiler.sh`, `scripts/flux_utility.sh` | Profiler and utility shell entry points | C | Adapted from Encore `encore_profiler` / `encore_utility` | Dual header; NOTICE §1 | Pending | Ships under Apache-2.0 with attribution |
| `scripts/fetch-synthesiscore.sh`, `scripts/verify-synthesiscore.sh`, `scripts/update-synthesiscore-lock.sh` | SynthesisCore dependency fetch/verify/lock tooling | A | Flux-authored; no Encore analogue | Flux copyright | N/A | Eligible |
| `module/*.sh` (`action`, `cleanup`, `customize`, `service`, `uninstall`, `verify`) | Magisk/KernelSU/APatch module lifecycle scripts | C | Adapted from Encore module scripts; operational branding (paths, process names) changed encore→flux | Dual header; NOTICE §1 | Pending | Ships under Apache-2.0 with attribution; operational branding is Flux-owned |
| `webui/src/**` | Module WebUI (Vue 3 SPA) | C | Adapted from Encore `encore_spa` WebUI; package renamed `encore_spa`→`flux_spa` | NOTICE §1 | **Pending** — full reconception as the independent "Flux Console" | Ships with attribution; not yet independent |
| `webui/src/main.js`, standard Vite/Vue bootstrap files | Framework bootstrap | B | Generic Vue/Vite scaffolding | — | Retain | Eligible |
| `gamelist.txt` | Game-detection package list | C | Data adapted from Encore's game list | NOTICE §1 | Pending — to be regenerated/curated independently | Ships with attribution |
| `jni/external/spdlog` | Logging library (submodule) | E | Upstream MIT | NOTICE §2 (spdlog) | Do not modify | Eligible |
| `jni/external/rapidjson` | JSON parsing (submodule) | E | Upstream MIT | NOTICE §2 (RapidJSON) | Do not modify | Eligible |
| ~~`webui/src/assets/fonts/googlesansflex-*.woff2`~~ (removed) | WebUI typeface | E (risk) — **resolved** | "Google Sans" is a proprietary Google typeface; redistribution terms are not Apache-2.0 | n/a (removed) | **Done** — assets and `@font-face` rules deleted; WebUI uses a system-first font stack and bundles no typeface. Rebuilt `dist` verified to contain zero font files | Eligible (no bundled font) |
| `webui/bun.lock` production deps (Vue, Vue Router, Pinia, Tailwind, kernelsu, webuix) | WebUI runtime dependencies | E | Predominantly MIT, preserved by their packages | NOTICE §3; SBOM at release | Do not vendor-modify | Eligible |
| `dependencies/synthesiscore.lock`, `prebuilt/synthesiscore.apk` | Pinned SynthesisCore artifact + checksum lock | A/E | SynthesisCore is a separate Flux-owned project consumed as a locked, checksum-verified artifact | NOTICE §4 | N/A | Eligible (checksum-gated in CI) |

## External reference-only material (Category E — reference, not consumed)

The following third-party projects are recorded for architectural risk
assessment only. **No source, asset, transaction list, class structure,
protocol, naming, or lifecycle from them may enter Flux** without explicit
licensing review and user approval. They are not dependencies and are not
distributed with Flux.

| Reference | What it is | Category | Flux position |
| --- | --- | --- | --- |
| Encore `binder_resolver` (https://github.com/Rem01Gaming/binder_resolver) | Standalone resolver that maps Binder Stub transaction field names to integer codes at runtime | E — reference-only | Not imported. Flux's own event-source design (see [`adr/0001-process-event-source.md`](adr/0001-process-event-source.md)) is written independently from AOSP behavior and public Binder docs. Any Flux `BinderContractProbe` is independently designed with an allowlist, validation, checksum/version metadata, and per-API fixtures. |
| Encore native Binder Monitor / BinderNDK work | Native Binder-based process/foreground monitoring in the Encore daemon | E — reference-only | Not copied. Flux's `ProcessEventSource`/`AndroidBinderTransport` is an independent design; Flux does not mirror its component hierarchy, protocol, or naming. |

SynthesisCore's long-term responsibility is deliberately **narrowed** to
device/sensor telemetry (thermal, Android thermal status, power saver, charging,
screen, audio, integer zen mode) plus a foreground *fallback*. Foreground /
process-lifecycle detection moves to the Flux-native `ProcessEventSource` in a
later phase. SynthesisCore must not become a monolithic process-monitoring or
policy service. See ADR 0001.

## Migration status

### Stage 1 — telemetry cutover: **complete**

Flux V2 telemetry is the **sole production telemetry implementation**. The legacy
implementation is not deprecated, not disabled, and not retained behind a flag — it is
deleted, and nothing links it.

What Stage 1 delivered, in order:

1. **Live cutover.** `TelemetryRuntime` is constructed and owned by the daemon and is the only
   telemetry authority. `Profiler` no longer owns a cache; it reads through an injected
   provider, so a second telemetry path cannot reappear without changing an explicit seam.
2. **Legacy removal.** `jni/base/SynthesisCore/*`, `ProfilePolicy.cpp`, the parity harness and
   the tests of the removed implementation are gone. No hidden fallback survives: there is no
   runtime path that reads the old JSON cache.
3. **Proof.** Build, link and package evidence rather than source-tree assertions (below).

**Behaviour is retained, not re-derived.** The removed `ProfilePolicyTest` /
`TelemetryParserTest` / `DecisionParityTest` are replaced by `DecisionVectorsTest` — golden
vectors whose expected values were captured from the V2 engine *while the parity harness was
still green*, i.e. the behaviour both implementations agreed on. The suite went from 175 to 127
tests; the entire reduction is tests *of the deleted implementation*, and every behaviour they
covered is still asserted.

### How the cleanliness claim is proven

Grep cannot prove what is in a binary, so the claim rests on the build's own artifacts. Each
layer is independent and fails the build on its own
(`.github/workflows/telemetry-proof.yml`):

| Evidence | Script | Proves |
| --- | --- | --- |
| Object manifest, archive members, binary symbol table, link identity | `.github/scripts/verify-native-telemetry.sh` | Every V2 component is linked into `fluxd` on every ABI; no legacy object, archive or symbol exists anywhere in the build; the stripped binary that ships is the same link output the symbols were proven on (build-id) |
| Flashable ZIP contents | `.github/scripts/verify-package.sh` | Expected ABIs; no build/source artifacts or key material; required attribution ships; the bundled APK is the pinned bytes; no Encore operational asset; no release output baked into the module |
| Pinned artifact metadata | `.github/scripts/verify-schema-provenance.sh` | The APK was built from the commit the lock names (read from the APK's own embedded build metadata, not its filename); it carries the complete v2 wire contract; the decoder requires v2 and admits no v1 |

Two deliberate limits are documented in the scripts themselves rather than glossed over:
`TelemetryStore` is header-only, so it is proven structurally through its owner instead of by a
symbol that inlining may legitimately erase; and the literal `SCHEMA_VERSION` constant is not
read out of the dex, because R8 inlines Kotlin `const val` into its call sites and leaves
nothing reliable to read.

### Scope — what Stage 1 is *not*

The Flux V2 rewrite as a whole is **not** complete. Stage 1 replaced the telemetry
implementation only. Still outstanding, and still Category C:

- **Stage 2 — Execution Engine live cutover.** `jni/engine/execution/` is written and tested
  (17 host tests) but the daemon still applies profiles through the legacy Encore-derived
  profiler path.
- Runtime Supervisor, Configuration Store, Diagnostics channel, Update Service (ADR 0002),
  Flux Console WebUI, and the Flux-native `ProcessEventSource` (ADR 0001).

The **provider-neutral foreground abstraction is retained for that future work**:
`RuntimeSnapshotAssembler::select_foreground(native, raw)` already prefers a native source and
falls back to SynthesisCore, so ADR 0001's `ProcessEventSource` can be introduced without
touching the decision path.

## Open compliance items (must close before declaring V2 independence)

1. ~~**Google Sans Flex** — unresolved redistribution license.~~ **Resolved:**
   font assets and `@font-face` rules removed; WebUI uses a system-first font
   stack and bundles no typeface (rebuilt `dist` verified font-free).
2. ~~**`jni/base/LockFile`** — provenance-unresolved (Category B/D).~~
   **Resolved:** replaced with an independent Flux implementation built on Linux
   OFD locks (`fcntl(F_OFD_SETLK)`), with an Apache-2.0/Flux header and host
   tests. See the LockFile row above.
3. All **Category C** components still ship under Apache-2.0 with Encore
   attribution. Attribution is retired *per component* only after independent
   reimplementation is validated and no derived code/assets/structure/expression
   remains — never merely because a file was renamed or partially rewritten.

## Attribution-retirement rule

For any Category C/D component, before removing its Encore attribution:

- the replacement is independently designed against the Flux V2 specification
  (not a line-by-line transform);
- behavior parity is proven by tests;
- the legacy runtime path is removed;
- a similarity check confirms no distinctive Encore expression remains.

Only then is the attribution removed from *that component*, and NOTICE.md
updated accordingly.
