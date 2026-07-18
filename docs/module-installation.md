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
| WebUI button | none — use Action | native | native |
| Action button | none | yes | yes |

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

Assets are generated, not hand-exported:

```
python3 scripts/render-banner.py     # module/assets/branding/banner.{svg,webp}  1280x640
python3 scripts/render-icons.py      # module/assets/icons/{action,donate}.{svg,webp}  192x192
```

Each script also takes `--check`, which regenerates into a temp directory and
compares against what is committed — rasters by decoded pixel content, SVG byte
for byte. Pixels rather than bytes because WebP encoding is not stable across
libwebp versions, and a check that goes red on a dependency upgrade while the
artwork is unchanged is one people learn to silence. CI runs both, so the packaged
artwork cannot drift from its source. `module/assets/` is the editable source tree and is
**not** packaged; the build copies each raster to the flat module-root path its
metadata key names.

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

> Device-specific vendor capabilities remain validation-gated. Safe generic
> behavior is used where a capability is not certified.

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

## Support and donations

| Manager | WebUI | Action button | `donate` metadata |
|---|---|---|---|
| Magisk | via Action | opens the WebUI | not read |
| KernelSU | native | opens Support | read by MMRL-family UIs |
| APatch | native | opens Support | read by MMRL-family UIs |
| MMRL | native card | defers to the card | read |

The Action button is spent on whatever the manager cannot already do. KernelSU and
APatch already have a WebUI button, so Action goes to Support. Magisk has no WebUI
button, so Action opens the WebUI — which means **Magisk users have no Support
entry point**, and Flux does not pretend otherwise.

**Flux currently has no official donation destination.** `OFFICIAL_DONATION_URL`
in `module/installer/config.sh` is empty, and while it is empty every donate path
is inert: no `donate` key is written into `module.prop`, no donation button is
claimed, and the Action button says plainly that there is nothing to open. To
enable it, set that one constant to an `https://` URL and rebuild.

Every URL Flux can open is a compile-time constant in that file. Nothing is read
from input, from a file on the device, from a property, or from the network, and
no donation page is ever opened during installation or boot.
