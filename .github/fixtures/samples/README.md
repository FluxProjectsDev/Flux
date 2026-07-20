# Captured installer output

Real transcripts, produced by `.github/scripts/verify-installer.sh` running the
installer against its fixture managers. They are **captures for review**, not
assertions — nothing diffs against them, because they carry values that move with
the build (`code 999`, file counts, the fixture's version string) and a golden that
reddens on an unrelated change is a golden people delete.

What *is* asserted lives next door and is the part worth pinning:

| Property | Where |
|---|---|
| the banner, byte for byte, per tier | `../banner-*.golden` |
| default block within 24 lines x 40 columns | `verify-installer.sh` §7 |
| the tall reference is not the default | `verify-installer.sh` §7 |
| one Flux success line, and only after real checks | `verify-installer.sh` §5 |

Regenerate after an intentional output change:

```sh
KEEP_FIXTURES=1 bash .github/scripts/verify-installer.sh
# then copy the relevant slices of <fixture-root>/case-*/install.log
```

## The files

| File | Shows |
|---|---|
| `install-01-first-screen.txt` | startup, the compact emblem, stages 1–2 |
| `install-02-middle-stages.txt` | stages 3–7 |
| `install-03-final-summary.txt` | stage 8 and the single success summary |
| `install-kernelsu-first-screen.txt` | the same opening under KernelSU |
| `install-apatch-first-screen.txt` | the same opening under APatch |
| `banner-narrow-30col.txt` | the 25-column tier, at `COLUMNS=30` |

## Reading the final summary

`[OK] Flux installed successfully.` is the only success line Flux emits.

A real flash also shows `Done`, `Installation complete` and
`Module installed successfully!`. Those are **not Flux's** — they come from
`install_module` in the manager's own `util_functions.sh`, invoked by
`META-INF/com/google/android/update-binary`, and they do not appear in these
captures because the fixture manager environment is a stub that does not implement
them. Flux does not suppress them: rewriting a manager's install chrome is how a
module breaks on the manager's next release.
