# Execution Engine migration: legacy shell → semantic intents

Status: in progress (Stage 2, Increment 3 — planning only, nothing is executed yet)

This document records how the legacy `scripts/flux_profiler.sh` behaviour maps onto the V2
execution engine, and — more importantly — where it deliberately does **not** map. It exists so
that the eventual cutover can be reviewed as a behaviour change rather than discovered as one.

`flux_profiler.sh` is still the only thing that applies a profile on a real device. Nothing in
this document is live.

## Why this is a re-expression, not a port

The legacy script fuses four jobs into one function call: it decides what profile to run, works
out which nodes exist, writes them, and reports success — with no seam between any of them. That
is why nothing about it can be tested before it runs on someone's phone, and why a half-applied
profile is indistinguishable from a whole one.

The V2 engine splits those jobs and keeps the split enforced in CI
(`.github/scripts/verify-provenance-boundary.sh`):

| Concern | Legacy | V2 | Category |
| --- | --- | --- | --- |
| What should be true | inline in each `*_performance` function | `PolicyIntent` / `CapabilityIntent` | A (Flux) |
| Which node expresses it | inline paths | `CapabilityDescriptor` | C (derived, attributed) |
| Whether it can be done here | `[ -f "$2" ]` at write time | `CapabilityProbe` → `CapabilityState` | A |
| What would happen | *unknowable until it happened* | `DryRunExecutionPlan` | A |
| Doing it | `apply()` / `write()` | `SysfsNodeBackend` (Increment 4) | A |

Mechanically translating the shell functions would have carried Encore's control flow into
Category A and collapsed the engine's independence claim. The device knowledge — which node, what
value, on which SoC — is what was retained, as attributed declarative data under `jni/device/`
with no logic in it.

## Behaviour mapping

Legacy profiles are per-SoC functions. V2 intents are outcomes; the SoC is a descriptor concern.

| Legacy entry point | Semantic intent | Behaviour class | Descriptor key |
| --- | --- | --- | --- |
| `performance_profile` → `<soc>_performance` | maximise sustained throughput | `SustainedPerformance` | `sustained_performance` |
| `performance_profile` (lite path) | throughput within a power bound | `ConstrainedPerformance` | `constrained_performance` |
| `balance_profile` → `<soc>_normal` | no active preference | `Balanced` | `balanced` |
| `powersave_profile` → `<soc>_powersave` | minimise draw | `PowerSave` | `safe` |
| `<soc>_normal` on screen-off / session end | give the original values back | `Restore` | `safe` |
| thermal handling (external to the script) | shed heat now, unconditionally | `Safe` | `safe` |

Six behaviour classes collapse to four descriptor keys on purpose. A node has no opinion about
*why* Flux wants it conservative — screen-off, battery saver and shutdown all mean the same thing
to a GPU frequency cap — so packs never enumerate policy concepts they cannot act on differently.
The distinction survives where it matters: in `PolicyIntent`, which the diagnostics path reads.

## What changes on purpose

These are behaviour differences, not translation gaps. Each one is a legacy behaviour the V2
engine declines to reproduce.

1. **Mode restoration stops guessing.** `apply()` ends with `chmod 444` regardless of what the
   mode was before, so a node that was `644` comes back `444`. V2 records the exact original mode
   from `fstat` on the open descriptor and restores that, on success *and* on failure, reporting
   `ModeRestoreFailed` explicitly rather than leaving the device altered and silent.

2. **Failures stop being invisible.** `apply()` sends every error to `/dev/null`: `EACCES` on a
   locked-down node and `EROFS` on a read-only mount are both "success". V2 distinguishes
   `PermissionDenied`, `ReadOnlyFilesystem`, `NotFound`, `SymlinkRejected` and
   `VerifyMismatch`, and a capability is executable only when it is `Supported`.

3. **Partial aggressive profiles are refused.** The legacy script applies whatever nodes happen to
   exist and reports the profile as applied. If a critical capability cannot execute, V2 withdraws
   the *whole* aggressive plan: a device left in a combination nobody designed, described as
   "performance", is worse than an unchanged device. Safety downgrades are never withdrawn this
   way — a missing performance knob must never block shedding heat.

4. **`chmod` targets a descriptor, not a path.** `change_cpu_gov` chmods a glob as root and writes
   through `tee`, so whatever the glob resolves to at that instant gets `644`. V2 opens with
   `O_NOFOLLOW`, checks the resolved path against an explicit allowlist of sysfs/procfs roots,
   rejects symlinks and non-regular files, and adds only `S_IWUSR` to the mode it actually read.

5. **Vendor capabilities start off.** Every derived descriptor ships as
   `PhysicalDeviceRequired` and is inert until probed and validated on real hardware of that
   family. The legacy script writes vendor paths on the assumption the SoC string implies them.
   Unvalidated is not deleted: the descriptor is present, gated, and reported as gated.

6. **"Active" became a claim with a standard.** A plan reports `would_be_fully_active` only when
   every intent — including the optional ones — was served by an executable descriptor. Optional
   means "do not withdraw the plan over it", not "pretend it happened".

## Staging

| Increment | Adds | Writes? |
| --- | --- | --- |
| 1 | `SysfsNodeBackend`: chmod-aware, allowlisted, verified writes | not called from production |
| 2 | `CapabilityState`, `DeviceDescriptor`, gated `DevicePacks` | no |
| 3 | `PolicyIntent`, `DryRunPlanner` — decision → fully specified plan | **no, structurally** |
| 4 | live atomic cutover behind the plan; legacy path retired after | yes |

Increment 3's inertness is enforced, not intended: `DryRunPlanner` holds a `const CapabilityProbe &`,
and CI fails the build if the planning sources so much as name `write_checked`, `fchmod`, `O_WRONLY`
or a `/sys` path literal.

`flux_profiler.sh` remains the live path and is not removed until Increment 4's replacement is
validated on hardware. Two active write paths must never exist at once, so the legacy path is
retired in the same change that makes the native one live — not before it, and not long after.

## Open items for Increment 4

- Atomic application: an ordered plan that stops at the first critical failure and rolls back.
- Restoration store: `RollbackStrategy::RestoreOriginal` needs the original values captured at
  apply time, not at plan time — a plan is a projection and may be stale by the time it runs.
- Hardware validation for each SoC family before its descriptors leave `PhysicalDeviceRequired`.
- Retiring `flux_profiler.sh` from `compile_zip.sh` in the same commit the native path goes live.
