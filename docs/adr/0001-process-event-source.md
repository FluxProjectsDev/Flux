# ADR 0001 — Process/foreground event source architecture

- **Status:** Accepted (design). Implementation deferred to a later Flux V2 phase.
- **Date:** 2026-07-15
- **Deciders:** FebriCahyaa
- **Supersedes:** none
- **Related:** [`provenance-matrix.md`](../provenance-matrix.md), the SynthesisCore
  telemetry contract v2.

## Context

Flux's Decision Engine needs to know, promptly and reliably, which application
is in the foreground and when game sessions start and end, in addition to the
device-state signals (thermal, power, screen, audio, zen) that SynthesisCore
already provides.

Today, foreground/package/PID/UID resolution is done inside SynthesisCore by
polling hidden `IActivityTaskManager` APIs on a 500 ms cadence (see
`app/.../AndroidProviders.kt::ForegroundAppProvider`). Polling has two costs:
latency (up to one poll interval of lag on a session change) and a standing
dependency on hidden, reflection-resolved framework APIs whose shape varies by
OEM ROM.

Separately, upstream reference information indicates the Encore project is
experimenting with a native, Binder-based process/foreground monitoring
architecture (a standalone `binder_resolver` that resolves Binder Stub
transaction field names to integer codes at runtime, plus native
BinderMonitor/BinderNDK components). **This ADR treats that work as external,
reference-only, Category E material for risk assessment.** No source, transaction
list, class structure, protocol, naming, or lifecycle from it may enter Flux.
See the provenance matrix. This ADR is written independently from AOSP interface
behavior, public platform Binder documentation, and Flux's own behavioral needs.

## Decision

Adopt **Approach B: an independent native Flux process-event source combined
with a reduced SynthesisCore sensor agent**, behind a capability gate, with the
Decision Engine consuming a single normalized `RuntimeSnapshot` that is
agnostic to which provider produced each field.

Responsibility split:

| Concern | Owner (primary) | Fallback |
| --- | --- | --- |
| Foreground package / PID / UID, game session start/stop | **Flux `ProcessEventSource`** (native, event-driven) | SynthesisCore foreground poll |
| Thermal headroom, Android thermal status, thermal sample age | SynthesisCore | — |
| Power saver, charging, screen, audio, integer zen mode | SynthesisCore | — |

SynthesisCore's long-term responsibility is **narrowed** to device/sensor
telemetry. It must not become a monolithic process-monitoring or policy service.
It retains a foreground *fallback* only, used when the native event source is
unavailable.

## Options considered

### A. SynthesisCore-only telemetry and foreground monitoring (status quo)

Keep polling foreground state inside SynthesisCore.

- **Pros:** simplest; already implemented and tested; one process; no new native
  surface; no Binder work.
- **Cons:** inherent poll latency on session changes; permanent reliance on
  hidden ATM APIs; foreground detection failures couple into the sensor agent.

### B. Native Flux `ProcessEventSource` + reduced SynthesisCore sensor agent (**chosen**)

A small, independent native component observes process/foreground lifecycle
event-driven, and publishes `ProcessLifecycleSnapshot` events into the Flux
engine. SynthesisCore keeps the sensor telemetry it is good at and provides only
a foreground fallback.

- **Pros:** low session-change latency; clean separation of concerns; the
  native source can fail closed to the SynthesisCore fallback without taking
  sensor telemetry down; each half is independently testable; keeps SynthesisCore
  narrow.
- **Cons:** introduces a native Binder-adjacent surface with real OEM/SELinux
  and API-compatibility risk; more moving parts; requires a capability gate.

### C. Fully native telemetry and process monitoring

Move both sensor telemetry and process monitoring into native code, retiring the
SynthesisCore JVM agent.

- **Pros:** single runtime; potentially lowest overhead.
- **Cons:** throws away a validated, host-testable Kotlin telemetry module;
  re-implements thermal/power/zen access (much of which is framework-level and
  awkward from native without the SDK); largest blast radius; highest risk;
  worst testability. Not justified by evidence.

## Consequences and dimension analysis (Approach B)

- **Latency.** Event-driven foreground detection removes up-to-one-poll-interval
  lag on session changes; the fallback path retains the existing ~500 ms poll
  latency. Sensor telemetry latency is unchanged.
- **CPU / wakeup overhead.** An event-driven source should reduce steady-state
  polling of ATM. It must not introduce a busy loop or frequent wakeups; the
  design target is to block on kernel/Binder readiness, not spin. Overhead is a
  measured acceptance criterion before the native source is promoted over the
  poll fallback.
- **Hidden API dependency.** Approach B *reduces* reliance on hidden ATM
  reflection for the common case, at the cost of a Binder-level dependency whose
  compatibility must be probed at runtime rather than assumed.
- **Binder transaction compatibility across Android versions.** Transaction
  codes are **not** stable ABI and differ across API levels and OEM builds.
  Flux must never hardcode a single universal transaction code. If runtime
  transaction resolution is required it is done by a `BinderContractProbe` (see
  below), independently designed, gated, and validated per API fixture.
- **OEM / SELinux compatibility.** Vendor SELinux policy may deny the native
  source access to the services or interfaces it needs. The source must detect
  denial and fail closed to the fallback, never crash-loop, and never assume
  availability.
- **Crash containment.** The native `ProcessEventSource` runs so that its failure
  degrades only foreground/session detection — the Decision Engine and
  SynthesisCore sensor telemetry continue. No provider failure may abort the
  whole runtime (the same isolation principle SynthesisCore's `TelemetryCollector`
  already enforces).
- **Capability detection.** A one-time probe at startup (and on relevant state
  changes) decides whether the native source is usable on this device/ROM/API.
  The result is surfaced as an explicit capability flag in diagnostics.
- **Fallback behavior.** Capability-gated, defined precisely below.
- **Testability.** The event-normalization and session logic live in a pure,
  host-testable layer (mirroring the `:telemetry` split), driven by recorded
  fixtures; the thin native transport is the only untestable-on-host part.
- **Security.** No arbitrary class/field/transaction lookup from user
  configuration. Inputs are validated; outputs are bounded. The probe operates
  against a fixed allowlist only.
- **Licensing / provenance.** The Flux implementation is written from scratch
  against AOSP behavior, public Binder documentation, and Flux's spec/tests.
  `binder_resolver` and Encore's Binder Monitor/BinderNDK work are recorded as
  Category E / reference-only; no material from them may be imported without
  explicit licensing review and user approval.

## Capability gate (required runtime behavior)

```
native ProcessEventSource available and permitted
    → event-driven foreground / process-lifecycle detection

native source unavailable or rejected (API/OEM/SELinux)
    → SynthesisCore foreground fallback (poll)

both unavailable
    → enter degraded state
    → the Decision Engine MUST NOT promote an aggressive performance profile
      (no foreground evidence ⇒ conservative/safe profile only)
```

Binder monitoring is **not** claimed to be universally supported across ROMs.

## Proposed Flux terminology (original; not mirroring any external hierarchy)

- **`ProcessEventSource`** — the abstraction the engine consumes; has native and
  fallback implementations behind one interface.
- **`AndroidBinderTransport`** — the thin native/Binder transport used by the
  native implementation.
- **`SessionEventRouter`** — turns raw process-lifecycle events into
  start/stop game-session events for the engine.
- **`BinderContractProbe`** — the independent, minimal, validated runtime
  resolver (only if runtime resolution proves necessary).
- **`ProcessLifecycleSnapshot`** — the normalized per-event record fed into the
  single `RuntimeSnapshot`.

Names may change if a better Flux-specific shape emerges; they will not be
chosen to match any external component naming.

## `BinderContractProbe` design constraints (if runtime resolution is needed)

Design only — not implemented in this phase. Any resolver must have:

- an **allowlist** of supported interfaces and fields (no open-ended lookup);
- **strict input validation** and **no** arbitrary class/field lookup sourced
  from user configuration;
- **bounded output**;
- explicit **Android/API metadata** attached to each resolution;
- **checksum and version metadata** for the resolved contract;
- **safe failure** (fail closed to the fallback, never crash);
- **tests across supported API fixtures**.

## Scope boundaries for this decision

- This ADR **decides** the architecture and division of responsibility.
- It **does not** implement the native source; that is a later, separately
  committed Flux V2 phase.
- The SynthesisCore V2 correctness work (thermal semantics, schema v2, provider
  isolation, tests, validation CI) is valid under all three approaches and
  proceeds independently of this decision.
