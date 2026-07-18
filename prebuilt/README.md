This folder contains following prebuilt programs:

[SynthesisCore](https://github.com/FluxProjectsDev/SynthesisCore): the telemetry provider Flux
consumes. `synthesiscore.apk` is not built here — it is a pinned release artifact, and which
release is pinned is recorded in [`dependencies/synthesiscore.lock`](../dependencies/synthesiscore.lock).

Do not replace this file by hand. The build verifies its bytes against the lock's checksum and
refuses to package anything that does not match. To move to a new release, use
`scripts/update-synthesiscore-lock.sh <tag> <path-to-apk>` and commit the artifact and the lock
together.
