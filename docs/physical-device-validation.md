# Physical-device validation checklist

Status: **no family is validated.** Every descriptor in `jni/device/` ships as
`ValidationStatus::PhysicalDeviceRequired` and therefore writes nothing. This document is the
procedure for changing that, one family at a time, and the record of what has been done so far.

Nothing here may be filled in from a datasheet, from the legacy shell script, or from reasoning
about what a node "should" do. Each line is a thing somebody observed on hardware they were
holding. That is the whole point: the device knowledge in `jni/device/` came from Encore Tweaks,
where it was presumably true of the devices its authors had — and "presumably true somewhere" is
exactly the claim this project stopped making.

## Rules

1. **A family is promoted per descriptor, not per SoC.** `SocFamily::MediaTek` is not a device.
   Two MediaTek phones can disagree about every node in the pack.
2. **Promotion requires a second reviewer** who did not run the tests, and a commit that names
   the exact device, kernel and ROM in its body.
3. **A failed line blocks that descriptor**, not the whole family. Leave it
   `PhysicalDeviceRequired` and record why.
4. **Never promote to make a test pass.** If Flux applies fewer writes than the legacy module on
   your device, that is the design working, not a bug to close.
5. **Re-validation is required** when a descriptor's path, value, allowlist, read-back strategy or
   rollback strategy changes. The old evidence was about the old descriptor.

## Per-run record

Copy this block into a validation report per device. Every field is required; "unknown" is a
valid answer and blocks promotion of anything that depends on it.

### Identity

| Field | Value |
| --- | --- |
| SoC model / revision | |
| Device codename | |
| ROM family / build | |
| Vendor fingerprint | |
| Android API level | |
| Kernel release (`uname -r`) | |
| GKI / KMI identifier | |
| Architecture (arm64-v8a / armeabi-v7a) | |
| Root manager (Magisk / KernelSU / APatch) + version | |
| Flux version + commit | |

### Capability — per descriptor

Run once per descriptor id in the family's pack. `flux_utility` can report the observed state;
the values still have to be read by a human who then writes them down.

| Check | Expected | Observed |
| --- | --- | --- |
| Node exists at the descriptor's path | present | |
| Path resolves without a wildcard surprise | single, stable match | |
| Node is a regular file (not a symlink or device node) | regular | |
| SELinux context | recorded | |
| Original mode (`stat -c %a`) | recorded, **not assumed 0444** | |
| Readable | yes | |
| Writable, or safely permission-adjustable | yes | |
| Current value format matches `NodeValueType` | matches | |
| Desired value is within range / allowlist | matches | |
| Read-back after write returns the written value | matches per `ReadBackStrategy` | |
| Original value captured before the first write | yes | |
| Rollback restores the exact original value | yes | |
| Original mode restored after success | exact | |
| Original mode restored after a forced failure | exact | |
| Critical-group completeness (all members executable) | complete | |

### Runtime — per device

| Check | Expected | Observed |
| --- | --- | --- |
| Daemon starts and holds its lock | yes | |
| Telemetry reaches Healthy | yes | |
| A profile request produces a plan | yes | |
| Constrained fallback when vendor gated | generic only, reported capability-limited | |
| Apply result is verified | `verified_active` | |
| Repeat of the same decision writes nothing | 0 writes | |
| Thermal downgrade beats an active game session | downgrade applied and verified | |
| Screen-off restores | originals restored | |
| Session end restores | originals restored | |
| Reboot leaves no Flux-owned value stranded | clean | |
| Uninstall restores the device | clean | |

### Safety — per device

Each of these is a thing the engine claims it does. A claim nobody has seen happen on hardware
is a claim, not a property.

| Check | Expected | Observed |
| --- | --- | --- |
| Symlinked target is rejected | `SymlinkRejected`, no write | |
| Path outside the approved roots is rejected | `PathNotAllowed`, no write | |
| Malformed desired value is rejected | plan rejected, no write | |
| Permission denied is reported, not swallowed | `PermissionDenied` | |
| Read-only filesystem is reported | `ReadOnlyFilesystem` | |
| Read-back mismatch fails the critical group | rollback attempted | |
| Rollback failure reports degraded | `rollback_failed`, no profile claimed | |
| External mutation is detected | verified claim dropped, re-applied | |
| Daemon restart re-verifies rather than assuming | re-applied | |
| Unsupported descriptor writes nothing | 0 writes | |
| Unknown kernel behaviour (node hangs/oops) | **stop; do not promote; record** | |

## Family status

| Family | Descriptors | Status | Evidence |
| --- | --- | --- | --- |
| `generic` | 3 (cpufreq policy0/4/7 governor) | **Flux-authored, not hardware-certified.** Executable in CI fixtures via `PhysicalDeviceValidated` *in tests only*; ships `PhysicalDeviceRequired` | none |
| MediaTek | 4 (`/proc/cpufreq`, `/proc/gpufreq`, `/proc/ppm`) | `PhysicalDeviceRequired` | none |
| Snapdragon | see `jni/device/DevicePacks.cpp` | `PhysicalDeviceRequired` | none |
| Exynos | see `jni/device/DevicePacks.cpp` | `PhysicalDeviceRequired` | none |
| Unisoc | see `jni/device/DevicePacks.cpp` | `PhysicalDeviceRequired` | none |
| Tensor | see `jni/device/DevicePacks.cpp` | `PhysicalDeviceRequired` | none |
| Tegra | 1 (`/sys/kernel/tegra_gpu/gpu_floor_rate`) | `PhysicalDeviceRequired` | none |

**Flux does not support MediaTek, Snapdragon, Exynos, Unisoc, Tensor or Tegra tuning today.**
It ships descriptors for them that are inert. Saying otherwise — in a release note, a README, or
a store listing — would be false.

The generic pack deserves its own caveat: it is Flux-authored from documented cpufreq behaviour
rather than derived, and the host tests promote it to `PhysicalDeviceValidated` to exercise the
engine. That promotion exists in test fixtures only. The shipped pack is gated like every other,
so a real device applies nothing until the generic descriptors are certified by this procedure
too.

## What a promotion commit looks like

```
feat(device): certify generic cpufreq governors on <device>

Validated on <SoC> / <codename> / <ROM build> / kernel <release>, API <n>,
<root manager> <version>, Flux <commit>.

Capability: all three policy governors present, regular files, mode 0644,
readable and writable without elevation, read-back exact, originals captured
and restored, modes restored on the failure path.

Runtime: verified apply, idempotent repeat (0 writes), thermal downgrade beat
an active session, screen-off and session-end restored, uninstall clean.

Safety: symlink rejected, traversal rejected, malformed value rejected,
read-only node reported, external mutation detected and re-applied.

Reviewed-by: <second reviewer who did not run the tests>
```
