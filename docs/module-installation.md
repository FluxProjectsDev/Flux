# Module installation

How Flux installs, what it verifies before it says so, and what each module manager
actually supports.

## Supported managers

| | Magisk | KernelSU | APatch |
|---|---|---|---|
| Minimum | v20.4+ | — | — |
| Android | 9 (API 28) or newer | same | same |
| Architecture | arm64-v8a, armeabi-v7a | same | same |
| Module mount | used | skipped (`skip_mount`) | skipped (`skip_mount`) |
| `fluxd` on `$PATH` | via the mount | symlinked into `/data/adb/ksu/bin` | symlinked into `/data/adb/ap/bin` |
| WebUI button | none | native | native |
| Action button | v27+ only | yes | yes |

An unrecognised manager installs too, provided it implements the standard module
contract (`MODPATH`, `ZIPFILE`, `TMPDIR`, `ARCH`, `API`, `ui_print`,
`set_perm_recursive`). If any of those is missing the install stops with a message
rather than failing halfway through. Flux never guesses a manager's private paths:
symlinks are only placed in directories the manager has already created.

APatch also sets `KSU=true`, so it is detected before KernelSU. Detection order is
load-bearing, not incidental.

## Branding

`banner`, `webuiIcon` and `actionIcon` are read by MMRL and WebUI X. **Magisk
ignores all three** and renders its stock module card — that is not a failure and
Flux does not claim otherwise.

The two kinds of asset have different provenance, and are proven differently.

**The banner is approved artwork, not generated.** `module/assets/branding/banner.webp`
(1280x720) is a direct conversion of a source the maintainer approved — decode,
proportional resize, WebP encode, nothing else. The approved original is supplied
out of band and is deliberately untracked (`.user-assets/` is git-ignored): it must
never reach the module ZIP. Do **not** redraw it, recolour it, or regenerate it from
coordinates. To change it, re-convert from the approved source and re-pin the hash in
`scripts/verify-branding.py`.

**The icons are generated:**

```
python3 scripts/render-icons.py      # module/assets/icons/{action,donate}.{svg,webp}  192x192
```

`--check` regenerates into a temp directory and compares against what is committed —
rasters by decoded pixel content, SVG byte for byte.

```
python3 scripts/verify-branding.py   # the banner's decoded identity + module.prop paths
```

Both use decoded pixels rather than file bytes, because WebP encoding is not stable
across libwebp versions, and a check that goes red on a dependency upgrade while the
artwork is unchanged is one people learn to silence. CI runs both, so the packaged
artwork cannot drift. `module/assets/` is the editable source tree and is **not**
packaged; the build copies each raster to the flat module-root path its metadata key
names.

## What you see while flashing

Eight numbered stages. The counter advances when a stage *starts*, so it can never
run ahead of the work:

```
[1/8] Package integrity              [5/8] Runtime and SynthesisCore payload
[2/8] Installation environment       [6/8] WebUI and module metadata
[3/8] Architecture and Android       [7/8] Permissions and configuration
[4/8] Existing installation          [8/8] Final verification
```

Every `[OK]` follows a check that ran. Extraction is followed by a SHA-256
comparison against the digest the build published; the installed runtime is
re-tested for its execute bit after permissions are applied (a filesystem can
accept `set_perm_recursive` and ignore it); the WebUI entry point is asserted to
exist; the generated `module.prop` is re-read from the installed tree and its
`id`, `name`, `version` and `versionCode` validated.

The installer opens with the Flux emblem — the swept "F" with its upper and lower
ribbons, dotted texture and outline `Flux` wordmark — followed by:

```
Adaptive Runtime Engine
Hardware-aware | Verified | Reversible
```

and then goes straight to real work. The art is a fixed, reviewed constant in
`module/installer/ui.sh` (a quoted heredoc, so nothing in it is expanded), held to
a byte-for-byte golden fixture in CI. Each tier is no wider than the width that
selects it, and the whole branding block — art, blank lines and strapline — is
counted, because that is what decides whether stage 1 lands on the first screen:

| Tier | Art | Widest line | Block | Selected when |
|---|---|---|---|---|
| verbose | 40 x 24 | 40 | 36 | `FLUX_BANNER_VERBOSE=1`, never automatic |
| default | 32 x 12 | 38 | 24 | `COLUMNS` unset, or >= 40 |
| compact | 22 x 16 | 25 | 22 | 25 <= `COLUMNS` < 40 |
| plain | 4 x 1 | 16 | 5 | `COLUMNS` < 25 |

The **default** tier is what a device shows: recovery and module managers
essentially never set `COLUMNS`, and an unset width means "not reported". It is a
proportional reduction of the reference art, not a truncation — the long upper
ribbon, left spine, diagonal inner cut, smaller lower ribbon, segmented outline and
down-left taper are all still present, with the dotted texture thinned so it reads
at half height.

The full-height `verbose` tier is kept as the reference form and is reachable only
by setting `FLUX_BANNER_VERBOSE=1`. It was previously the default, at which point
the branding block was 36 lines — most of two screens on a phone, so stage 1 opened
below the fold and the first thing a user saw of an install was scrollback. CI
asserts the default block stays within 24 lines by 40 columns, and that the
full-height art is not what the default path emits.

Captured transcripts are in `.github/fixtures/samples/`.

There are no sleeps, no spinners and no progress animation. Output is plain ASCII
with `[*]`/`[OK]`/`[WARN]`/`[FAIL]` markers, switching to Unicode only when a
UTF-8 locale is declared — an unset locale, which is normal in recovery, means
"unknown", not "probably fine". No colour is emitted at all: Magisk's `ui_print`
writes through the updater protocol, and managers that render it in a WebView or
a log file would show the escape bytes literally.

## Three outcomes

**SUCCESS** — every critical requirement passed.

**SUCCESS WITH LIMITATIONS** — installed and will run, but something optional is
unavailable and is named in the log. Causes include an unrecognised manager, a
missing branding asset, no manager `bin` directory to symlink into, or a
configuration that had to be reset.

An unidentified SoC family is *not* one of them. Flux gates vendor capability
behind runtime certification regardless, so an unrecognised family simply means
the runtime uses the safe generic behaviour the summary already promises — nothing
the user installed is missing, and reporting it as a limitation would be crying
wolf on a healthy install.

**Aborted** — a critical requirement failed. Nothing is kept, temporary files are
removed, and no success line is printed. This is structural rather than a
convention: the abort path does not return, so the summary is unreachable after
one.

Fatal conditions include an unsupported architecture, Android older than 9, a
checksum mismatch on any critical payload, a missing runtime/WebUI/SynthesisCore,
a tampered installer component, and a malformed `module.prop`.

## No vendor claims during flash

Flux identifies the SoC family and writes it for the runtime to read. It applies
no tuning and enables no vendor capability. Capability certification happens on
the device, in the execution engine, against the actual node set — long after the
installer is gone. Every install therefore ends with:

> Device-specific capabilities remain validation-gated.
> Reboot to start Flux.

A device with no certified vendor capability is **not** a failed installation.

## Clean install and upgrade

Your configuration is yours. On upgrade it is preserved, not "refreshed":
packaged defaults are applied per file, so a file you already have is never
overwritten, while genuinely new defaults still land.

A configuration the daemon cannot parse is backed up to
`gamelist.json.invalid` before defaults are restored, and the install reports a
limitation. It is never silently used and never silently deleted.

Upgrades also remove artifacts earlier versions left *outside* the module
directory — chiefly the pre-V2 `flux_profiler` symlink in the manager's `bin`
directory, which replacing the module cannot clean up on its own. Only
specifically named paths are removed; there is no wildcard deletion.

## Uninstall

Removes Flux-owned state only: `/data/adb/.config/flux`, the cleanup hook in
`/data/adb/service.d`, and the `fluxd` / `flux_utility` / `flux_profiler`
symlinks from manager `bin` directories. `flux_profiler` stays on that list even
though Flux no longer ships it — installs from before the V2 cutover created it,
and uninstall has to clean up what previous versions left behind.

Other modules and unrelated user data are untouched, and a partial or legacy
installation is tolerated.

## Module card buttons

| Button | Does |
|---|---|
| left Action | runs the Flux Self-Test |
| WebUI | opens the Flux WebUI |
| `$` | opens the official SociaBuzz donation page |
| delete | uninstalls the module |

The `$` button is driven by the `donate` key in `module.prop`, which the build
writes from `OFFICIAL_DONATION_URL`. The manager opens it; no Flux script is
involved, which is why Action no longer carries it — the donation page already had
its own button, and spending Action on a duplicate wasted the one control that had
no other home.

**Magisk renders almost none of this.** It ignores `banner`, `webuiIcon`,
`actionIcon` and `donate`, has no WebUI button, and only gained module Action
support in v27 — while Flux installs on v20.4+. So on Magisk the card offers the
self-test on v27+, and nothing at all below that.

Magisk users reach the WebUI through a standalone viewer (WebUI X or
KSUWebUIStandalone), not from the module card. Action previously tried to open the
WebUI for them; that path is gone. On v27+ it is a real trade — the WebUI shortcut
was replaced by the self-test. Below v27 nothing changed, because an Action button
that does not exist could never have invoked it.

## The Action button: Flux Self-Test

Tapping Action runs a bounded, read-only self-test and prints the result into the
manager's log window. It is the answer to "is Flux actually working?", which
nothing else on the card could tell you.

```
Flux Self-Test
----------------------------

[PASS] Module metadata
[PASS] Flux runtime binary (arm64-v8a)
[PASS] Flux daemon active
[PASS] SynthesisCore telemetry (2s old)
[PASS] Telemetry schema v2
[WARN] Vendor capabilities require device validation
[PASS] WebUI assets
[PASS] Rollback support
[PASS] Legacy profiler absent

Result: PASS WITH LIMITATIONS
```

Seven groups are checked: module installation, Flux runtime, SynthesisCore
telemetry, runtime health, device capability, WebUI and package assets, and safety.

| Result | Meaning | Exit |
|---|---|---|
| `PASS` | every critical check passed, nothing gated | 0 |
| `PASS WITH LIMITATIONS` | critical checks passed; optional or gated items unavailable | 2 |
| `FAIL` | at least one critical component failed | 1 |

`PASS WITH LIMITATIONS` is the expected result on every device today, because
vendor capabilities are gated behind physical-device validation and that gate is
reported as a `WARN`. A gated capability is **not** a failure: the execution engine
plans nothing for a capability it has not validated, so a gated device performs
zero vendor writes by construction.

### What it does not do

The self-test only reads. It writes nothing, applies no profile, touches no sysfs
node, calls no execution-engine apply, alters no `RuntimeProfileState`, starts and
restarts no service, opens no URL, and sends nothing off the device. CI asserts
this rather than trusting it: a fixture takes a content-and-mode manifest of the
module and config trees before and after a run and requires them identical, and a
stubbed Activity Manager must record no intent.

Every value it reads comes off the filesystem and is treated as data, never as
code. There is no `eval`, no constructed command, and no network tool in the
script. Injection fixtures feed `$(...)`, backticks, `;`, `&&` and `|` payloads
through `module.prop`, `soc_recognition`, `current_profile` and the telemetry
snapshot, and assert that a canary file never appears.

### The telemetry contract

The snapshot at `/data/adb/.config/flux/synthesis_core.json` is the canonical path, and all four
components agree on it: `Flux.hpp`'s `SYNTHESIS_CORE_FILE`, `Main.cpp`, `service.sh` (which passes
it to SynthesisCore), and `webui/src/stores/Monitor.js`.

**Despite the `.json` name, the wire format is not JSON.** Schema v2 is line-oriented
`key<SPACE>value`, split on the *first* space only:

```
schema_version 2
sequence 1487
focused_package com.example.game
```

`schema_version` is a top-level field, its position in the file is not significant, and the
accepted band is exactly v2 (`kSchemaMin` = `kSchemaMax` = 2). Duplicate keys are rejected
outright, a trailing CR per line is stripped, and input is bounded at 64 KiB / 4 KiB per line /
256 lines.

SynthesisCore writes temp → fsync → rename, so the target is never partially visible. A truncated
or empty snapshot is therefore a real fault, not a race — which is why the self-test does not
retry.

The self-test is shell and cannot link `TelemetryDecoder`, so it re-implements the tokenizer. That
duplication once shipped a bug: it parsed `key=value` and reported a healthy device as
`Telemetry snapshot malformed (no schema_version)`, and the fixtures were written in the same
wrong format so they agreed with the bug. The corpus in `.github/fixtures/telemetry/` is now the
single source of truth, read by **both** parsers — `jni/tests/TelemetryContractTest.cpp` decodes it
with the production decoder, and `verify-installer.sh` §6 runs the self-test over it, asserting the
same classification for all ten fixtures. Change the format, the tokenizer or the bounds, and one
of the two goes red.

Each state is reported as itself rather than collapsed: absent, permission-denied, empty,
wrong-dialect (`key=value`), no schema field, non-numeric schema, legacy (below v2), unsupported
(above v2), duplicate key, and stale.

### Honest gaps

Degraded, rollback-failed, external-mutation and capability-limited states are
reported as **not exported** rather than as passing. This runtime publishes only
`current_profile` (see `docs/status-contract.md`), so there is no evidence on disk
for those states — and a `PASS` there would assert that rollback succeeded and
nothing was mutated externally on no evidence at all. If a future runtime writes
the `runtime_status.json` that the status contract names as the seam, the self-test
already reads it: a failed rollback is critical, mutation and capability-limited
are warnings.

## Donations

The official destination is <https://sociabuzz.com/fbrichy>.

`OFFICIAL_DONATION_URL` in `module/installer/config.sh` is the single definition.
Setting it writes the `donate`/`donateIcon` keys into `module.prop` and points the
WebUI's support entry at the same address; clearing it back to `""` removes both,
and no donation button is claimed anywhere.

Because a WebUI page cannot source a shell file, `webui/src/views/Home.vue`
restates the same URL, and CI asserts the two agree so they cannot drift into
sending users to different addresses.

Every URL Flux can open is a compile-time constant. Nothing is read from input,
from a file on the device, from a property, or from the network, and no donation
page is ever opened during installation, boot, or by the self-test — only a
deliberate tap on the manager's own `$` button.
