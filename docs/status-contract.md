# Runtime status contract — WebUI/CLI assessment

Status: **honest but limited.** What Flux exposes today is accurate; it is a strict subset of
what `RuntimeProfileState` knows.

This is an audit, not a redesign. It records exactly which runtime facts reach the WebUI, proves
the ones that do cannot be read as a false claim, and names the ones that do not yet — so a
future Flux Console can add them without having to rediscover what is missing.

## What the runtime models

`RuntimeProfileState` (see `jni/engine/execution/RuntimeProfileState.hpp`) already distinguishes
every state the increment requires:

| Concept | Field / value |
| --- | --- |
| requested profile | `requested_profile()` |
| effective constrained profile | `effective_profile()`, `effective_intent()` |
| last verified profile | `verified_profile()`, `has_verified_profile()` |
| capability-limited | `ApplyState::CapabilityLimited` |
| device-validation required | `ApplyState::…` + `device_validation_pending()` |
| unsupported | `ApplyState::Unsupported` |
| degraded | `ApplyState::Degraded` |
| rollback in progress | `ApplyState::RollbackInProgress` |
| rollback failed | `ApplyState::RollbackFailed`, `rollback_failed()` |
| external mutation | `ApplyState::ExternalMutation` |
| tweaks disabled | `RuntimeTuning::tweaks_enabled` (gate), no verified profile |
| generic fallback in use | `CapabilityLimited` + a verified generic apply |
| fully optimized | `fully_optimized()` — true only when nothing was prevented or skipped |
| degraded reason | `degraded_reason()` |
| telemetry / capability health | `telemetry_health()`, `capability_health()` |
| last successful apply time | `last_verified_apply_ms()` |

The `ApplyHistoryEntry` ring buffer additionally carries per-apply action counts, prevented-action
counts, and a sanitized error category (never a path or an errno). The data exists, in memory,
per cycle.

## What reaches the WebUI today

Exactly one field: the **verified** profile, written to `current_profile` by the runtime's status
publisher (`jni/Main.cpp`, via `profile_mode_from_target(verified_profile, has_verified_profile)`)
and read by `webui/src/stores/Home.js` and `Monitor.js`. `gameinfo` carries the active package.

The important property is what that publisher writes: the *verified* profile, not the requested
one. Before the V2 cutover the legacy path wrote the profile it had asked the shell to apply,
whether or not it took. Now, until an apply verifies, `current_profile` reports the safe default.

## Why the current contract cannot lie

§6 lists claims the WebUI must never make. Each is impossible today, and impossible for the
specific reason that the field which would enable it is not exposed:

| Forbidden claim | Why it cannot happen |
| --- | --- |
| full vendor optimization while vendor is blocked | there is no "optimization level" field; the WebUI shows a profile, and a gated vendor pack still produces a verified *generic* profile, not a "fully optimized" flag |
| active performance mode from requested config alone | `current_profile` is the verified profile; a requested-but-unverified performance ask reports the safe default |
| successful apply without verification | same — the publisher writes `verified_profile`, and `has_verified_profile()` is false until a critical group verifies |
| Supported capability while `PhysicalDeviceRequired` | capability state is not exposed to the WebUI at all; a gated capability simply does not appear as an applied profile |
| successful rollback when rollback failed | rollback state is not exposed; a failed rollback leaves `has_verified_profile()` false, so `current_profile` does not advance |
| active governor choice when the capability was rejected | a rejected governor never verifies, so it never becomes the verified profile |

The contract is honest by omission: the WebUI cannot overclaim because it is not given the fields
an overclaim would need. That is a safe limitation, not a bug — but it *is* a limitation.

## The presentation limitation

A user on an unvalidated device (every device today) sees `current_profile` report Balanced when
the runtime is actually in `CapabilityLimited` — generic cpufreq applied, vendor tuning gated.
That is not wrong, but it is thin: the WebUI cannot yet tell the user *why* the profile is what it
is, that vendor tuning is pending hardware validation, that a rollback degraded, or that an
external change was detected.

Those facts all exist in `RuntimeProfileState` and `ApplyHistoryEntry`. What is missing is a
machine-readable export of them and a UI that renders them. Both are **out of scope for Stage 2**
and belong to the Flux Console redesign and the Incident Recorder / Diagnostics Channel named in
the roadmap. No field has been lost; the richer state is preserved in the runtime model, awaiting
a consumer.

## Recommendation for the next increment

When the Diagnostics Channel is built, the runtime's `StatusPublisher` is the single seam to
extend: it already receives the full `RuntimeProfileState` on every cycle. Serialising the table
above to a `runtime_status.json` beside `current_profile` would expose everything the WebUI needs
without changing a single decision or write path — the status publisher writes files, it does not
apply anything. Until then, `current_profile` remaining the verified profile is the honest
minimum, and it holds.
