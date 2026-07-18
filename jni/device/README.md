# `jni/device/` — declarative device packs (Category C)

**This directory holds data, not code.** Read this before adding anything to it.

## What this is

Per-SoC tuning knowledge, expressed as `flux::execution::CapabilityDescriptor` values: a path,
a type, a range or allowlist, a value per policy intent, a read-back strategy, a rollback
strategy, and a provenance label. That is all a descriptor can be — the schema has nowhere to
put anything else.

## Why it is separate from `jni/engine/execution/`

Two different things with two different origins, and the separation is the point.

| | `jni/engine/execution/` | `jni/device/` |
| --- | --- | --- |
| Contains | the mechanism: backend, registry, validator, planner, engine, probe | the knowledge: which node, what value |
| Provenance | **Category A** — Flux-authored, clean-room | **Category C** — derived from Encore Tweaks |
| Attribution | Flux copyright only | dual copyright + NOTICE §1 |
| Writes to a device | yes, and it is the only thing that does | **never** |

The tuning knowledge in this directory is derived from
[Encore Tweaks](https://github.com/Rem01Gaming/encore) (Apache-2.0, Copyright © 2024–2026
Rem01Gaming) — specifically the per-SoC blocks of `scripts/flux_profiler.sh`, which is itself
adapted from Encore's profiler. It carries that attribution, and must keep carrying it until
the relevant table has been independently re-derived and validated. See
[`docs/provenance-matrix.md`](../../docs/provenance-matrix.md).

Keeping it here, as data, is what lets the execution engine stay Category A. If device
knowledge were expressed as C++ control flow inside the engine — as it was expressed as shell
control flow in the profiler — then the engine would inherit that provenance, and the
independence the V2 rearchitecture exists to establish would be lost for the one component
that most needs it.

## Rules

A descriptor **must not** contain:

- a shell command, an `eval`, or anything that is executed;
- a callback, function pointer, or any executable content;
- a path assembled at runtime, or a path outside the approved kernel virtual roots;
- a mode to `chmod` to — permission handling belongs to `SysfsNodeBackend`, which grants the
  minimum bit and restores the exact original mode;
- policy-selection logic — *what* profile to pick is the Decision Engine's job; a descriptor
  only says what a given intent means for one node;
- a URL, or anything downloadable.

`DescriptorValidator` enforces these, `.github/scripts/verify-provenance-boundary.sh` enforces
the directory-level boundary in CI, and both fail the build rather than a review.

## Every descriptor here is inert until proven

Each ships `ValidationStatus::PhysicalDeviceRequired`. `CapabilityProbe` refuses to mark
anything `Supported` below `PhysicalDeviceValidated`, so **these tables currently produce no
writes at all**. Host tests and NDK builds cannot promote them: they prove a descriptor is
well-formed and that the node exists, not that writing it does what the table claims on real
silicon.

Promotion to `PhysicalDeviceValidated` requires someone running it on real hardware of that
family and confirming the node takes the value and the effect is the intended one. Until then
the family is *experimental / device validation required*, and saying otherwise in a release
note would be a claim nobody has checked.
