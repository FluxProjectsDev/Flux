# Flux Changelog

## v1.0.0

### Initial Independent Release

This release establishes **Flux** as an independent adaptive runtime
optimization project under **FluxProjectsDev**.

Flux is no longer presented as “Flux Tweaks”. The project now has its own
product identity, native runtime architecture, execution model, telemetry
contract, packaging workflow, and safety guarantees.

### Core Runtime

- Introduced the native **Flux Adaptive Runtime Engine**.
- Added deterministic runtime decision processing.
- Added semantic policy intents instead of direct shell-based profile
  commands.
- Added capability-aware planning for CPU, GPU, power, thermal, Zen, and
  other supported runtime controls.
- Added immutable execution plans.
- Added one authoritative production write path through
  `ExecutionEngine`.
- Removed the legacy profiler dispatcher and legacy automatic profile
  shell application path.
- Removed `flux_profiler.sh` from the production source tree and module
  package.
- Added verified `RuntimeProfileState` for reporting the last successfully
  applied state.

### Safe Execution

- Added transactional node application.
- Added preflight validation before changes are written.
- Added read-back verification after each critical write.
- Added rollback and original-value restoration.
- Added external mutation detection.
- Added idempotent execution to avoid repeated identical writes.
- Added strict path allowlisting and symlink rejection.
- Added safe permission handling with original mode restoration.
- Added exact Zen mode handling for modes `0`, `1`, `2`, and `3`.
- Unsupported and unvalidated capabilities now produce zero writes.

### Device and Capability Awareness

- Added declarative device and SoC capability descriptors.
- Added explicit capability states such as:
  - `Supported`
  - `Unsupported`
  - `Unavailable`
  - `PermissionDenied`
  - `InvalidFormat`
  - `VerificationFailed`
  - `PhysicalDeviceRequired`
- Added constrained generic behavior for devices without validated vendor
  support.
- Added physical-device validation requirements for vendor-specific
  tuning.
- Added initial descriptor families for generic, Snapdragon, MediaTek,
  Exynos, Unisoc, Tensor, and Tegra environments.
- Vendor-specific capabilities remain disabled until runtime probes and
  physical-device validation succeed.

### SynthesisCore Integration

- Integrated the independent **SynthesisCore** telemetry provider.
- Added versioned telemetry schema support.
- Added atomic telemetry snapshot publication.
- Added freshness and malformed-data validation.
- Added provider isolation so one failing provider does not stop the
  complete telemetry service.
- Added corrected thermal headroom semantics.
- Added safe handling for unavailable and non-finite telemetry values.
- Added locale-independent numeric serialization.
- Added compatibility checks between Flux and SynthesisCore artifacts.

### Profiles and Runtime Behaviour

- Added adaptive performance profiling.
- Added foreground and game-aware profile decisions.
- Added battery-aware and thermal-aware profile constraints.
- Added distinct Balanced, Performance, and Power Save semantics.
- Preserved user governor selection through the V2 semantic configuration
  path.
- Added `disable_tweaks` as a verified no-write mode.
- Added `NO_PERFORMANCE_CPUGOV` mitigation for kernels that cannot safely
  use aggressive CPU governors.
- Thermal safety and stale telemetry can override performance promotion.

### Module Packaging

- Supports Magisk, KernelSU, and APatch module environments.
- Includes native runtime binaries for:
  - `arm64-v8a`
  - `armeabi-v7a`
- Includes the validated SynthesisCore APK.
- Includes the local WebUI and module lifecycle scripts.
- Added clean-install, legacy-upgrade, reinstall, and uninstall fixtures.
- Added cleanup for obsolete Flux profiler symlinks and payloads.
- Added generated `module.prop` validation.
- Added deterministic package inventory and SHA-256 reporting.
- Added package checks that prevent removed legacy payloads from returning.

### Installation Experience

- Added the approved Flux banner artwork for module managers that render
  `banner`.
- Added a Flux ASCII identity during module installation.
- Added truthful numbered installation stages.
- Success indicators are shown only after actual validation.
- Added clear warning, limitation, and fatal-error states.
- Added integrity checks for the runtime binary, SynthesisCore APK, WebUI,
  configuration, lifecycle scripts, and module metadata.
- Added Support and Donate handling where the active module manager
  supports it. The official destination is
  `https://sociabuzz.com/fbrichy`; it is a compile-time constant, is never
  taken from input, and is never opened during installation or boot.
- Added branding validation that pins the banner's decoded pixel identity,
  so packaged artwork cannot silently drift or be replaced by generated
  substitutes.

### WebUI

- Includes local WebUI access for runtime configuration and status.
- WebUI assets are packaged locally and do not require a CDN.
- WebUI cannot directly execute arbitrary shell commands or write
  arbitrary system paths.
- Runtime status is based on verified state rather than requested profile
  alone.
- A complete Vue 3 and Material Design 3 redesign is planned for a later
  release.

### Diagnostics and Quality

- Added bounded execution history.
- Added explicit rejection and capability-limited states.
- Added host tests, Android ABI builds, package lifecycle fixtures, and
  production binary proofs.
- Added ASan and UBSan validation.
- Added static analysis, ShellCheck, workflow validation, package security
  scans, and provenance checks.
- Added proof that `ExecutionEngine` is the only production profile write
  entry point.
- Added proof that legacy profiler symbols and payloads are absent from
  production binaries and module packages.

### How Flux Differs from Encore

Flux began with historical inspiration and device knowledge associated
with Encore, but this release introduces a substantially different
product architecture and runtime model.

Key differences include:

- Flux uses a native decision, planning, and execution pipeline.
- Flux does not use Encore’s legacy automatic profile shell application
  flow.
- Flux separates telemetry collection into SynthesisCore.
- Flux uses semantic policy intents and immutable execution plans.
- Flux verifies writes through read-back before reporting a profile as
  active.
- Flux supports transactional rollback and original-value restoration.
- Flux treats device and SoC definitions as declarative data rather than
  executable tuning scripts.
- Flux blocks unvalidated vendor capabilities instead of assuming device
  paths are safe.
- Flux distinguishes requested, constrained, verified, degraded, and
  rollback states.
- Flux includes independent package, lifecycle, security, and runtime
  contract validation.
- Flux is maintained under the `FluxProjectsDev` organization with its own
  branding, repositories, CI/CD, and release infrastructure.

Some historical device-specific knowledge remains attributed where
required. Legal notices, provenance records, and upstream acknowledgements
are preserved in `LICENSE`, `NOTICE.md`, and the project provenance
documentation.

### Known Limitations

- No vendor or SoC family is considered universally validated.
- Device-specific capabilities may remain
  `PhysicalDeviceRequired`.
- Unvalidated devices may receive fewer writes than older legacy
  implementations.
- Generic constrained behavior is intentional when device support cannot
  be proven safely.
- The current WebUI does not yet expose every detailed runtime rejection
  reason.
- The packaged `actionIcon` and `webuiIcon` are not yet the official Flux
  emblem, and are pending the approved transparent two-ribbon source. The
  `banner` is the approved artwork.
- Physical-device certification and expanded device packs will be added
  progressively.
- GPU driver switching, OpenGL/Vulkan routing, and Flux Lab benchmarking
  are not included in this initial release.

### Repository

- Flux:
  `https://github.com/FluxProjectsDev/Flux`
- SynthesisCore:
  `https://github.com/FluxProjectsDev/SynthesisCore`
