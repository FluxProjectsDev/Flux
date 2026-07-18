#!/system/bin/sh
#
# Copyright (C) 2026 FebriCahyaa
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Installer output primitives.
#
# Design constraints, all of them learned from what recovery and module-manager consoles
# actually do rather than from what a desktop terminal can do:
#
#   - NO colour, ever. Magisk's ui_print writes through the updater's OUTFD protocol; a manager
#     that renders that log in a WebView or writes it to a file shows the raw escape bytes
#     instead of interpreting them. Colour buys nothing here and corrupts the log when it fails.
#   - NO cursor movement, no carriage returns, no screen clearing. The transcript must stay
#     readable after the fact, and a manager that captures it line-by-line cannot replay a
#     redraw. This rules out spinners and in-place progress bars; progress is expressed as
#     numbered stages, which are honest in a log and need no terminal capability at all.
#   - NO sleeps. Nothing here delays installation to make it look like work is happening.
#   - ASCII by default, Unicode only on positive evidence (see flux_ui_init). Recovery shells
#     routinely run with no locale set, and a mojibake status marker is worse than a plain one.
#
# The status markers are the load-bearing part of this file. Every one of them is emitted by a
# stage that has already performed a check — flux_step_ok is never called merely because a
# command returned. See finalize.sh for the post-install assertions that back the final summary.

FLUX_UI_MARK_STEP="[*]"
FLUX_UI_MARK_OK="[OK]"
FLUX_UI_MARK_WARN="[WARN]"
FLUX_UI_MARK_FAIL="[FAIL]"
FLUX_UI_RULE="---------------------------------------------"

FLUX_STAGE_TOTAL=8
FLUX_STAGE_CURRENT=0
FLUX_WARN_COUNT=0

# ui_print is provided by the module manager's installer environment. The fallback exists so the
# helpers can be sourced and exercised by the CI lifecycle fixtures, which have no Magisk.
if ! command -v ui_print >/dev/null 2>&1; then
	ui_print() { echo "$1"; }
fi

# Decide whether Unicode markers are safe.
#
# Positive evidence only: a UTF-8 locale must be *stated*. An unset locale is the common case in
# recovery and means "unknown", not "probably fine" — so it takes the ASCII path. FLUX_FORCE_ASCII
# lets the fixtures pin the ASCII branch regardless of the host's locale.
flux_ui_init() {
	if [ -n "${FLUX_FORCE_ASCII:-}" ]; then
		return 0
	fi
	case "${LC_ALL:-}${LC_CTYPE:-}${LANG:-}" in
	*UTF-8* | *utf8* | *UTF8* | *utf-8*)
		FLUX_UI_MARK_STEP="•"
		FLUX_UI_MARK_OK="✔"
		FLUX_UI_MARK_WARN="⚠"
		FLUX_UI_MARK_FAIL="✖"
		;;
	esac
}

# ── Identity ────────────────────────────────────────────────────────────────
# The Flux emblem, matching the approved reference (the swept "F": long upper ribbon, vertical
# left spine, diagonal inner cut, smaller lower ribbon, dashed outline, horizontal dotted texture
# in both ribbons) with the outline mixed-case "Flux" wordmark beneath it.
#
# FIXED CONSTANTS, not generated. The art below is authored with scripts/compose-ascii-logo.py
# (a development tool, never packaged and never run on a device) and pasted here once, reviewed.
# .github/scripts/verify-installer.sh holds the emitted output to a byte-for-byte golden file, so
# a stray edit to a single space fails CI rather than shipping a broken logo.
#
# The heredoc delimiter is QUOTED ('FLUX_ASCII_LOGO'). That is load-bearing: it disables
# parameter expansion, command substitution and backslash processing, so the many `\` and `/`
# glyphs in the diagonals survive verbatim. An unquoted delimiter would eat every backslash and
# silently shred the art.
#
# `IFS= read -r` matters for the same reason: IFS= keeps the leading spaces that position every
# line, and -r stops read from consuming backslashes.
#
# One further hazard, checked rather than assumed: Magisk's ui_print writes with `echo -e`, which
# interprets \a \b \c \e \f \n \r \t \v \\ and \0. The art deliberately contains no such
# sequence — every backslash in it is followed by a space, a slash, an underscore or a line end —
# and verify-installer.sh asserts that, because one `\b` added later would eat a character on
# every real device while looking perfect in a fixture that echoes plainly.

FLUX_BANNER_WIDTH=40
FLUX_BANNER_COMPACT_WIDTH=25

flux_banner_detailed() {
	while IFS= read -r _flux_line; do
		ui_print "${_flux_line}"
	done <<'FLUX_ASCII_LOGO'
        ________________________________
      -'  . . . . . . . . . . . . . . \
     /  . . . . . . . . . . . . . . .\
    /  . . . . . . . . . . . . . . .\
   /  . . . . . . . . . . . . . . \
  /  . . . . . . . . . . . . . . \
 |  . . . . . . . . . . . .  ---\
 |        -------------------'
 |       /
 |      /
 |     /     _____________________
 |   /     /  . . . . . . . . . \
 |  /     / . . . . . . . . . \
 | /     / . . . . . . . . . \
 |     / . . . . . . . . .  \
 |    /          ---------'
 |   /       ----
 | . . . . /
 | . . .  /
 | . . ./
 |. . /
| . ./
|. ./
| /

  _____  _
 |  ___|| |  _   _  __  __
 | |__  | | | | | | \ \/ /
 |  __| | | | | | |  \  /
 | |    | | | |_| |  /  \
 |_|    |_|  \__,_| /_/\_\
FLUX_ASCII_LOGO
}

flux_banner_compact() {
	while IFS= read -r _flux_line; do
		ui_print "${_flux_line}"
	done <<'FLUX_ASCII_LOGO_COMPACT'
     _________________
   -' . . . . . . . \
  /  . . . . . . . \
 /  . . . . . . . \
 | . . . . . . --\
 |    ---------'
 |   /
 |  /   ___________
 |/   /  . . .   \
 |   /  . . .   \
 |   /    -----'
 |  /  ---
| .  /
| /

Flux
FLUX_ASCII_LOGO_COMPACT
}

# The detailed emblem is the default. The narrow variants are selected only when the console
# ACTUALLY reports a width too small for it — an unset or zero COLUMNS means "not reported",
# which is the norm in recovery and in module managers, and takes the detailed art.
#
# Each tier's WIDEST line, art and strapline together, is no wider than the threshold that
# selects it. That is the whole contract of a fallback, and it is easy to get wrong here: the
# one-line strapline is 38 columns, so a narrow tier that used the 22-column emblem and then
# printed the full strapline underneath would wrap anyway and have accomplished nothing. The
# narrow tiers therefore break the strapline up as well.
#
#   tier      art  widest line  selected when
#   detailed   40      40       COLUMNS unset, or >= 40
#   compact    22      25       25 <= COLUMNS < 40
#   plain       4      16       COLUMNS < 25
flux_print_banner() {
	ui_print ""
	_cols="${COLUMNS:-0}"
	if [ "${_cols}" -le 0 ] || [ "${_cols}" -ge "${FLUX_BANNER_WIDTH}" ]; then
		flux_banner_detailed
		ui_print ""
		ui_print "Adaptive Runtime Engine"
		ui_print "Hardware-aware | Verified | Reversible"
	elif [ "${_cols}" -ge "${FLUX_BANNER_COMPACT_WIDTH}" ]; then
		flux_banner_compact
		ui_print ""
		ui_print "Adaptive Runtime Engine"
		ui_print "Hardware-aware | Verified"
		ui_print "Reversible"
	else
		ui_print "Flux"
		ui_print ""
		ui_print "Adaptive Runtime"
	fi
	ui_print ""
}

# ── Structure ────────────────────────────────────────────────────────────────
flux_section() {
	ui_print ""
	ui_print "$1"
	ui_print "${FLUX_UI_RULE}"
}

# flux_step_begin <text>
# Increments the stage counter and prints "[n/8] text". The counter is the progress indicator:
# it advances only when a stage actually starts, so it cannot run ahead of the work.
flux_step_begin() {
	FLUX_STAGE_CURRENT=$((FLUX_STAGE_CURRENT + 1))
	ui_print ""
	ui_print "[${FLUX_STAGE_CURRENT}/${FLUX_STAGE_TOTAL}] $1"
}

# flux_step_ok <text>
# Call ONLY after the corresponding condition has been tested. This function does not check
# anything; it reports that the caller did.
flux_step_ok() { ui_print "  ${FLUX_UI_MARK_OK} $1"; }

flux_step_warn() {
	FLUX_WARN_COUNT=$((FLUX_WARN_COUNT + 1))
	ui_print "  ${FLUX_UI_MARK_WARN} $1"
}

flux_step_fail() { ui_print "  ${FLUX_UI_MARK_FAIL} $1"; }

flux_info() { ui_print "  ${FLUX_UI_MARK_STEP} $1"; }

# ── Termination ──────────────────────────────────────────────────────────────
# flux_abort <reason> [detail ...]
#
# The single fatal exit. Prints the failure, runs the cleanup hook so no half-extracted payload
# is left in TMPDIR or MODPATH, and aborts. It never prints a success line, and no summary can
# follow it: `abort` does not return.
flux_abort() {
	_reason="$1"
	shift
	ui_print ""
	ui_print "${FLUX_UI_RULE}"
	flux_step_fail "${_reason}"
	while [ "$#" -gt 0 ]; do
		ui_print "      $1"
		shift
	done
	ui_print ""
	ui_print "  Installation aborted. No changes were kept."
	ui_print "${FLUX_UI_RULE}"

	if command -v flux_cleanup_temp >/dev/null 2>&1; then
		flux_cleanup_temp
	fi

	if command -v abort >/dev/null 2>&1; then
		abort ""
	fi
	exit 1
}

# ── Final summary ────────────────────────────────────────────────────────────
# flux_summary <status>   status: SUCCESS | LIMITED
#
# There is no FAILED case here on purpose. A fatal failure goes through flux_abort, which never
# returns, so this function is unreachable after one. That is the structural guarantee behind
# "a critical failure cannot print success": it is not a flag that could be forgotten, it is the
# fact that the success path is downstream of an exit.
flux_summary() {
	ui_print ""
	ui_print "${FLUX_UI_RULE}"
	if [ "$1" = "LIMITED" ]; then
		ui_print "  ${FLUX_UI_MARK_WARN} Flux installed with limitations."
		ui_print ""
		ui_print "  ${FLUX_WARN_COUNT} optional item(s) reported a warning above."
		ui_print "  The module is installed and will run; the items"
		ui_print "  listed are optional and are unavailable."
	else
		ui_print "  ${FLUX_UI_MARK_OK} Flux installed successfully."
	fi
	ui_print ""
	# Said on every install, including the clean one. Flux gates vendor-specific capability
	# behind runtime validation on the device; the installer has not applied any tuning and must
	# not imply that it has.
	ui_print "  Device-specific vendor capabilities remain"
	ui_print "  validation-gated. Safe generic behavior is used"
	ui_print "  where a capability is not certified."
	ui_print ""
	ui_print "  Reboot to start the Flux runtime."
	ui_print "${FLUX_UI_RULE}"
	ui_print ""
}
