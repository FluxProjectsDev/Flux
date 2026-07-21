# Release verification and hardening

This document describes what Flux's release hardening does and does not protect, and how to
verify an official Flux build. It is written for users deciding whether to trust a downloaded ZIP
and for maintainers operating the release.

## What this is not

**Flux does not claim to be impossible to reverse engineer, and does not try to be.** The native
runtime is stripped and its symbols are minimized, the WebUI is minified, and the shell is not
obfuscated — but none of that prevents anyone determined from reading what Flux does. That is by
design: Flux is Apache-2.0 software with an obligation to remain auditable (see
[`NOTICE.md`](../../NOTICE.md)), and a module that runs as root has *more* reason to be readable,
not less. There are deliberately **no** encrypted shell payloads, no decrypt-and-eval, no
self-modifying code, no runtime-downloaded executable code, no anti-tamper that punishes the
device, no device lock-in, no account binding, and no DRM.

Hardening here means something narrower and honest: raise the cost of *casual copying and
tampering*, keep secrets and build-host details out of the shipped artifact, and make an
unmodified official build *verifiable* — so a user can tell a genuine release from a re-packaged
one.

## What release hardening protects

| Area | What it does | What proves it |
| --- | --- | --- |
| Native binary | Stripped; symbols hidden via an export map; RELRO, immediate binding, non-exec stack, PIE; no developer/CI paths embedded | `.github/scripts/verify-native-hardening.sh` |
| Information leakage | No `/home/...`, no tokens, no source paths in the shipped binary or package | native-hardening + `verify-release-readiness.sh` |
| WebUI | Minified; no source maps; no dev/test files; no remote code; no secrets | `.github/scripts/verify-webui-inventory.sh` |
| Runtime integrity | Critical files checksummed at install and re-checked at boot | `.github/scripts/verify-runtime-integrity.sh` |
| Supply chain | Least-privilege CI, no secrets on pull requests, pinned/reported actions | `.github/scripts/verify-supply-chain.sh` |
| Secrets | No credentials in source or package | `scan-secrets.sh` + `verify-release-readiness.sh` |
| Provenance | Checksum manifest + attestation tying the ZIP to the build | `gen_release_metadata.sh` + attestation |

## Verifying a download

Every official release ships three files beside the flashable ZIP: `SHA256SUMS`,
`build-metadata.json`, and a build-provenance attestation.

**1. Check the checksums.** Download the ZIP and `SHA256SUMS` into the same directory, then:

```sh
sha256sum -c SHA256SUMS
```

Every line must say `OK`. A mismatch means the file is not the one the build produced — do not
flash it.

**2. Verify the provenance attestation.** The attestation is a signed statement that *this*
repository's build workflow produced *this exact* ZIP. With the GitHub CLI:

```sh
gh attestation verify flux-<version>.zip --repo FluxProjectsDev/Flux
```

A successful verification confirms the ZIP's digest matches an attestation signed for a run of
Flux's own build workflow. The build pipeline additionally proves, in CI, that the attested
subject digest equals the digest of the produced ZIP — so the attestation cannot describe a
different file than the one published.

**3. (Optional) Read the metadata.** `build-metadata.json` records the source commit, the workflow
run URL, the toolchain, the ABIs, and the digests of the ZIP and the bundled SynthesisCore APK.
The digests are deterministic; the toolchain, run identity and timestamp are environment-dependent.
Flux does **not** claim bit-for-bit reproducibility until it has been independently verified.

## What happens on an integrity failure

At install, the module verifies every packaged file against its published digest and records the
digests of the installed critical files as a runtime baseline. At each boot — before any device
node is touched — Flux re-checks that baseline.

If a critical file was replaced, removed, symlinked away, turned into a directory, made
group/other-writable, or the package generation no longer matches, Flux **enters a safe no-write
state**:

- it applies no tuning and starts no writer, so nothing is changed on the device;
- it records a bounded reason (a class and one filename, never a digest);
- it **does not** delete files, reboot, or bootloop, and it never attempts destructive "repair".

The failure is surfaced where a user already looks:

- the module manager's **Action** button (Self-Test) reports the exact failure class and reason;
- the WebUI Home page shows an integrity banner.

The threat model is explicit: this defends against post-install replacement by a non-root actor or
a botched update. A root actor can rewrite the baseline too and a module manager can disable Flux
outright, so root tampering is out of scope by construction.

## Key and symbol custody (for maintainers)

- **Signing keys are never in the repository.** Provenance uses GitHub's keyless (OIDC/Sigstore)
  attestation, so there is no long-lived private key to store or leak, and no placeholder or
  committed key is ever used to sign. Any future signing material stays in the release
  environment's secret store, out of the source tree — `scan-secrets.sh` fails CI if key material
  is ever committed.
- **Debug symbols are retained privately.** The shipped binary is stripped; the unstripped link
  output (with `.symtab` and a matching GNU build-id) is uploaded as a short-retention CI artifact
  that is **never** a release asset. Maintainers pull it to symbolize a crash address; the symbols
  that were stripped off the release stay off the release while remaining recoverable.

## Reporting suspected tampering

If `sha256sum -c` fails, `gh attestation verify` fails, or Self-Test reports a runtime integrity
failure you did not cause, open an issue at the official tracker
(<https://github.com/FluxProjectsDev/Flux/issues>) and include the Self-Test output (it is safe to
screenshot — it carries no device identifier, no telemetry payload, and no secret) and where you
obtained the download. Do not flash a build that fails verification.
