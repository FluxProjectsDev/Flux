# ADR 0003 — Flux Adaptive Runtime Engine: descriptor boundaries

**Status:** Accepted as forward constraints. The descriptor foundation landed in Stage 2
Increment 2; the adaptive behaviour it is designed for is **not implemented and not scheduled
here.**

This ADR records *boundaries and roadmap only*. It exists so the descriptor model does not get
designed into a corner, not to authorise building the engine it anticipates.

## Why the descriptor model has the shape it does

The legacy applier expressed device knowledge as executable shell. Three consequences drove
this design, and every constraint below traces to one of them:

1. **Nothing could be checked before it ran.** A wrong path or a wrong value was discovered on
   a user's phone. There was no way to validate, diff, or dry-run device knowledge without a
   device.
2. **Every new device meant new privileged code.** Device support and write mechanism were the
   same artifact, so extending the first always meant touching the second.
3. **Provenance was contagious.** Because the knowledge *was* the code, anything that consumed
   it inherited its provenance. That is precisely why the execution engine had to be built as a
   mechanism that knows nothing about any device.

Data can be validated, reviewed, and attributed. Code that does the same job cannot be, cheaply.

## Forward constraints

These must hold for anything built on this foundation. They are cheap now and expensive later.

**Descriptors stay data.** No callbacks, no commands, no runtime-assembled paths, no chmod
modes, no URLs. If a capability appears to need one, that is a signal it belongs in the
Category A engine as a new *mechanism* — never smuggled into a pack. `DescriptorValidator` and
`.github/scripts/verify-provenance-boundary.sh` enforce this in CI.

**Versioned, and refused whole when unknown.** `schema_version` is mandatory. Packs outlive the
code that reads them; a pack declaring an unimplemented schema is rejected entirely rather than
partially understood, because a half-understood tuning descriptor is a wrong write.

**Capability-first, never identity-first.** SoC identity makes a descriptor a *candidate*.
Probing decides. No capability is ever enabled because a name matched — that assumption is how
a device gets tuned with another device's values.

**Only `Supported` acts.** Every other state produces no chmod and no write, and prevents the
owning profile from being reported fully active. There is deliberately no state meaning
"probably fine, try it anyway".

**Aggressive groups are all-or-nothing.** A critical descriptor that is not executable withholds
its whole group. Half of a tuning set leaves the device in a combination nobody designed.

**Unvalidated means inert.** A descriptor below `PhysicalDeviceValidated` never acts. Host tests
and NDK builds prove a descriptor is well-formed and that a node exists — never that writing it
does what the table claims. Only hardware clears that, per family.

**One write path.** `SysfsNodeBackend` is the only component that writes a device node, and it
is replaceable behind `NodeBackend`. No device-specific write mechanism may ever live inside
`ExecutionEngine`.

**Rejections stay structured.** Reasons are enumerated, not prose. The future Diagnostics
Channel must not have to parse sentences.

**The engine stays device-agnostic.** `jni/engine/execution/` must never include a device table
or reference `flux::device`. The dependency runs one way, and CI checks it.

## Roadmap (not authorised here)

- **Per-family physical-device validation.** The blocking prerequisite for any stable support
  claim. Until a family is validated, it is *experimental / device validation required*, and a
  release note saying otherwise would be a claim nobody has checked.
- Device packs as loadable data rather than compiled-in tables, once the schema has survived
  contact with real hardware.
- Adaptive intent selection informed by RuntimeSnapshot history.
- Per-pack attribution retirement, only after a family's table is independently re-derived and
  validated — see the attribution-retirement rule in `docs/provenance-matrix.md`.

## Explicit non-goals for this increment

Policy Graph, predictive thermal modelling, native process monitoring (ADR 0001), and the live
execution cutover itself. Increment 2 delivers the descriptor foundation and nothing else: no
descriptor in this repository can currently produce a write.
