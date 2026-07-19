#!/system/bin/sh
#
# Copyright (C) 2024-2026 Rem01Gaming
# Copyright (C) 2024-2026 FebriCahyaa
#
# Adapted from Encore Tweaks (https://github.com/Rem01Gaming/encore).
# Modified by the Flux project.
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
# The module manager's Action button: a bounded, read-only Flux self-test.
#
# The Action button used to open the donation page. It no longer does — managers that support
# donation have a dedicated `$` button which opens the same approved URL from module.prop's
# `donate` key, so spending Action on it duplicated a control the user already had. Action is now
# spent on the one thing no other button offers: telling the user whether their install is
# actually working.
#
# WHAT THIS SCRIPT MAY DO: read files, stat files, read properties, ask whether a process exists.
# WHAT IT MUST NEVER DO, and does not:
#   - write, create, delete or chmod anything, anywhere;
#   - apply a profile, touch a sysfs node, or call into the execution engine;
#   - alter RuntimeProfileState, or start/stop/restart any service;
#   - upload, transmit or otherwise send anything off the device;
#   - open a URL, including the donation URL;
#   - eval, or build a command out of anything it read.
#
# Every check below is a test, a read, or a bounded probe. There is no code path here that opens a
# file for writing. That is the property to preserve when editing this file.
#
# Portability: Android's /system/bin/sh is mksh or toybox ash depending on the ROM, and a module
# manager may invoke this under something else again. POSIX only — no `local` (a shell extension;
# helper variables are `_`-prefixed instead so they cannot collide), no arrays, no [[ ]], no
# process substitution, no $'...'.
#
# Exit codes, because a manager or a script may want to act on the outcome:
#   0  PASS                    every critical check passed, nothing gated
#   2  PASS WITH LIMITATIONS   critical checks passed; optional or gated items unavailable
#   1  FAIL                    at least one critical component failed

MODDIR="${0%/*}"

# Test seams. They default to the real device paths, exactly as the installer's do, so the shipped
# behaviour is unchanged and the fixtures can point the whole self-test at a temp tree.
FLUX_MODULE_DIR="${FLUX_MODULE_DIR:-${MODDIR}}"
FLUX_CONFIG_DIR="${FLUX_CONFIG_DIR:-/data/adb/.config/flux}"

# Telemetry the runtime publishes for the WebUI. The schema version is the contract between
# SynthesisCore, fluxd and the WebUI; see webui/src/stores/Monitor.js, which pins the same number.
FLUX_TELEMETRY_SCHEMA=2
FLUX_TELEMETRY_FILE="${FLUX_CONFIG_DIR}/synthesis_core.json"

# Freshness bound, in seconds. Deliberately looser than the WebUI's 5 s: that is a live monitor
# refreshing on a timer, this is a one-shot probe a user taps by hand, and a 5 s bound here would
# report "stale" on a perfectly healthy device that simply had not ticked yet.
FLUX_TELEMETRY_MAX_AGE="${FLUX_TELEMETRY_MAX_AGE:-60}"

# Every external command is bounded. A wedged `pm` or a stuck binder call must not hang the
# manager's Action window open forever.
FLUX_PROBE_TIMEOUT="${FLUX_PROBE_TIMEOUT:-5}"

FLUX_SYNTHESIS_PKG="com.febricahyaa.synthesiscore"

_pass_count=0
_warn_count=0
_fail_count=0

# Telemetry diagnostic state, filled in by check_synthesiscore and printed by the diagnostic
# block at the end. Defaults describe "never got that far", so a run that aborts early reports
# unknown rather than inheriting a stale value.
_tel_raw=""
_tel_state="unknown"
_tel_boot="unknown"
_tel_format="unknown"
_tel_lines=""
_tel_age=""
_schema=""

# ── Reporting ────────────────────────────────────────────────────────────────
# Labels are kept under ~44 columns for the same reason the installer's are: this output is read
# on a phone, inside a module manager's log window, and a wrapped status line is hard to scan.
st_pass() {
	_pass_count=$((_pass_count + 1))
	echo "[PASS] $1"
}

st_warn() {
	_warn_count=$((_warn_count + 1))
	echo "[WARN] $1"
}

st_fail() {
	_fail_count=$((_fail_count + 1))
	echo "[FAIL] $1"
}

st_note() {
	echo "       $1"
}

# probe <command> [args...]
# Runs a read-only external command under a timeout when one is available. `timeout` is present in
# toybox on modern Android but not on every ROM, so its absence degrades to a direct call rather
# than to a skipped check.
probe() {
	if command -v timeout >/dev/null 2>&1; then
		timeout "${FLUX_PROBE_TIMEOUT}" "$@" 2>/dev/null
		return $?
	fi
	"$@" 2>/dev/null
}

# prop_value <key> <file>
# Reads one `key=value` line. This is module.prop's format, and ONLY module.prop's format.
# Pure shell, no eval: the value is never interpreted, only printed.
prop_value() {
	[ -f "$2" ] || return 1
	sed -n "s/^$1=//p" "$2" 2>/dev/null | head -1
}

# tel_field <key>
# Reads one field from the telemetry snapshot held in ${_tel_raw}.
#
# THE SNAPSHOT IS NOT key=value, AND IT IS NOT JSON. Despite the file being named
# synthesis_core.json, the schema-v2 wire format is line-oriented `key<SPACE>value`, and the split
# is on the FIRST space only, so values may themselves contain spaces. Both production consumers
# do exactly this and are the contract:
#
#   jni/engine/telemetry/TelemetryDecoder.cpp   line.find(' '), key = [0,sp), value = (sp,end]
#   webui/src/stores/Monitor.js                 trimmed.indexOf(' '), same split
#
# Using a `key=value` reader here is what made the self-test report a healthy device as
# "Telemetry snapshot malformed (no schema_version)": the sed found no `schema_version=` because
# the real line is `schema_version 2`. The fixtures were written in the same wrong format, so
# they agreed with the bug instead of catching it. .github/fixtures/telemetry/ now holds ONE
# corpus in the real format, and both this parser and the production C++ decoder are tested
# against it — see jni/tests/TelemetryContractTest.cpp.
#
# Matching "${key} " as a prefix is equivalent to the first-space split, because a key never
# contains a space: the producer emits fixed identifiers.
tel_field() {
	printf '%s\n' "${_tel_raw}" | {
		while IFS= read -r _tf_line; do
			case "${_tf_line}" in
			"$1 "*)
				printf '%s' "${_tf_line#"$1 "}"
				return 0
				;;
			esac
		done
		return 1
	}
}

# ── 1. Module installation ───────────────────────────────────────────────────
check_module() {
	_prop="${FLUX_MODULE_DIR}/module.prop"
	if [ ! -f "${_prop}" ]; then
		st_fail "Module metadata (module.prop missing)"
		return
	fi

	_id="$(prop_value id "${_prop}")"
	_name="$(prop_value name "${_prop}")"
	_ver="$(prop_value versionCode "${_prop}")"

	if [ "${_id}" != "flux" ]; then
		st_fail "Module identity (id=${_id:-<empty>}, expected flux)"
	elif [ "${_name}" != "Flux" ]; then
		st_fail "Module identity (name=${_name:-<empty>}, expected Flux)"
	elif [ -z "${_ver}" ]; then
		st_fail "Module metadata (versionCode missing)"
	else
		st_pass "Module metadata"
	fi

	# The lifecycle scripts the manager and the installer rely on. uninstall.sh in particular is
	# checked under §7 as well, because its absence is a safety problem and not merely a missing
	# file: it is the user's way out.
	_missing=""
	for _s in service.sh uninstall.sh action.sh cleanup.sh; do
		[ -f "${FLUX_MODULE_DIR}/${_s}" ] || _missing="${_missing} ${_s}"
	done
	if [ -n "${_missing}" ]; then
		st_fail "Lifecycle scripts missing:${_missing}"
	else
		st_pass "Lifecycle scripts present"
	fi
}

# ── 2. Flux runtime ──────────────────────────────────────────────────────────
# elf_machine <file> — e_machine from the ELF header, as two lowercase hex bytes.
# Bytes 18-19, little-endian: "b7 00" is AArch64, "28 00" is 32-bit ARM. Reading two bytes of a
# header is cheaper and far more honest than trusting the path the binary was installed from.
elf_machine() {
	command -v od >/dev/null 2>&1 || return 1
	od -A n -t x1 -j 18 -N 2 "$1" 2>/dev/null | tr -d ' \n'
}

check_runtime() {
	_bin="${FLUX_MODULE_DIR}/system/bin/fluxd"
	if [ ! -f "${_bin}" ]; then
		st_fail "Flux runtime binary (fluxd not installed)"
		return
	fi
	if [ ! -s "${_bin}" ]; then
		st_fail "Flux runtime binary (fluxd is empty)"
		return
	fi
	if [ ! -x "${_bin}" ]; then
		st_fail "Flux runtime binary is not executable"
		return
	fi

	# The installed binary must match the device it is installed on. An armeabi-v7a fluxd on an
	# arm64 device would fail to exec at boot with nothing in the manager log to explain it.
	_abi="$(probe getprop ro.product.cpu.abi)"
	_mach="$(elf_machine "${_bin}")"
	if [ -z "${_mach}" ]; then
		st_pass "Flux runtime binary"
		st_note "ABI not verified (od unavailable)"
	else
		case "${_abi}:${_mach}" in
		arm64-v8a:b700 | armeabi-v7a:2800 | armeabi:2800)
			st_pass "Flux runtime binary (${_abi})"
			;;
		:*)
			st_pass "Flux runtime binary"
			st_note "device ABI not reported; skipped ABI match"
			;;
		*)
			st_fail "Runtime ABI mismatch (device ${_abi})"
			;;
		esac
	fi

	# Daemon liveness. `pidof` is the cheap path; a `ps` scan is the fallback. Neither starts,
	# signals or otherwise disturbs the process — this asks whether it exists and nothing more.
	_alive=1
	if command -v pidof >/dev/null 2>&1; then
		probe pidof fluxd >/dev/null 2>&1 && _alive=0
	elif command -v ps >/dev/null 2>&1; then
		probe ps -A 2>/dev/null | grep -q "fluxd" && _alive=0
	else
		_alive=2
	fi
	if [ "${_alive}" = "0" ]; then
		st_pass "Flux daemon active"
	elif [ "${_alive}" = "2" ]; then
		st_warn "Flux daemon state unknown (no ps/pidof)"
	else
		# Not fatal on its own: the module can be installed correctly and simply not have been
		# started yet, which is exactly the state between flashing and rebooting.
		st_warn "Flux daemon not running (reboot pending?)"
	fi

	# Runtime paths the daemon and the installer agree on.
	if [ -d "${FLUX_CONFIG_DIR}" ]; then
		st_pass "Runtime paths present"
	else
		st_fail "Runtime config dir missing"
		st_note "${FLUX_CONFIG_DIR}"
	fi
}

# ── 3. SynthesisCore ─────────────────────────────────────────────────────────
check_synthesiscore() {
	_apk="${FLUX_MODULE_DIR}/synthesiscore.apk"
	if [ ! -s "${_apk}" ]; then
		st_fail "SynthesisCore payload missing"
	else
		# An APK is a ZIP. Two bytes of magic distinguishes a real payload from a truncated or
		# placeholder file, which would otherwise only surface as a boot-time crash loop.
		_magic="$(od -A n -t c -N 2 "${_apk}" 2>/dev/null | tr -d ' \n')"
		if [ -n "${_magic}" ] && [ "${_magic}" != "PK" ]; then
			st_fail "SynthesisCore payload is not an APK"
		else
			st_pass "SynthesisCore payload"
		fi
	fi

	# Package identity. If the package manager knows it, that is the authoritative answer; if not,
	# the identity the launcher will actually use is the constant in service.sh, and a drift
	# between that and the expected package is the failure this catches.
	if command -v pm >/dev/null 2>&1 && probe pm path "${FLUX_SYNTHESIS_PKG}" >/dev/null 2>&1; then
		st_pass "SynthesisCore package identity"
	elif grep -q "${FLUX_SYNTHESIS_PKG}" "${FLUX_MODULE_DIR}/service.sh" 2>/dev/null; then
		st_pass "SynthesisCore package identity"
	else
		st_fail "SynthesisCore package identity mismatch"
	fi

	# ── The telemetry contract ───────────────────────────────────────────────
	# Each distinguishable state is reported as itself. Collapsing them loses the one piece of
	# information that tells a user whether to reboot, reflash, or report a bug.
	#
	# SynthesisCore writes temp -> fsync -> rename (see AtomicStatusWatcher.hpp). A rename is
	# atomic, so the target is NEVER partially visible: a reader sees the whole old file or the
	# whole new one. That is why there is no retry loop here — a truncated or empty target is a
	# real fault, not a race that waiting would resolve.
	if [ ! -e "${FLUX_TELEMETRY_FILE}" ]; then
		# Not published yet. Degraded, not broken: Flux runs with fewer inputs, and a reboot
		# fixes it. Reporting FAIL would send a user to reflash over a pending boot.
		st_warn "Telemetry snapshot not published yet"
		_tel_state="absent"
		return
	fi
	if [ ! -r "${FLUX_TELEMETRY_FILE}" ]; then
		st_fail "Telemetry snapshot unreadable (permission denied)"
		_tel_state="denied"
		return
	fi
	if [ ! -s "${FLUX_TELEMETRY_FILE}" ]; then
		# Zero bytes behind an atomic writer means the producer wrote nothing, not that a write
		# is in flight.
		st_fail "Telemetry snapshot is empty"
		_tel_state="empty"
		return
	fi

	# Bounded read: the production decoder rejects anything over 64 KiB, so reading more than that
	# could only produce a verdict the runtime would not agree with. CR is stripped because the
	# decoder strips a trailing CR per line.
	_tel_raw="$(head -c 65536 "${FLUX_TELEMETRY_FILE}" 2>/dev/null | tr -d '\r')"
	if [ -z "${_tel_raw}" ]; then
		st_fail "Telemetry snapshot unreadable"
		_tel_state="unreadable"
		return
	fi

	# Detect the DIALECT the producer is actually speaking, and report it whatever the outcome.
	#
	# This exists because two physical devices reported the same telemetry failure and neither
	# could be inspected from here. The contract is `key<SPACE>value`, established from the two
	# production consumers (TelemetryDecoder.cpp and Monitor.js) — but SynthesisCore is a prebuilt
	# APK that builds its lines with character appends, so its separator cannot be read out of the
	# binary to confirm it independently. If a device ever disagrees, this line says so directly
	# instead of costing another round trip.
	#
	# Only the SHAPE is reported: a delimiter class and a key name. Key names are fixed producer
	# identifiers, never user data, and no value is ever printed.
	_tel_first="$(printf '%s\n' "${_tel_raw}" | sed -n '/./{p;q;}')"
	case "${_tel_first}" in
	'{'* | '['*) _tel_format="json" ;;
	*[a-z_]" "*) _tel_format="space" ;;
	*[a-z_]"="*) _tel_format="key=value" ;;
	*"$(printf '\t')"*) _tel_format="tab" ;;
	'') _tel_format="blank" ;;
	*) _tel_format="unknown" ;;
	esac
	_tel_lines="$(printf '%s\n' "${_tel_raw}" | grep -c .)"

	# The production decoder rejects a duplicate key outright (DecodeError::DuplicateKey) rather
	# than taking the first or the last. A shell reader that silently took the first would call a
	# snapshot healthy that the runtime itself refuses — the self-test would then disagree with
	# the thing it exists to report on.
	_tel_dupe="$(printf '%s\n' "${_tel_raw}" | sed -n 's/^\([^ ][^ ]*\) .*/\1/p' |
		sort | uniq -d | head -1)"
	if [ -n "${_tel_dupe}" ]; then
		st_fail "Telemetry has a duplicate key (${_tel_dupe})"
		_tel_state="duplicate"
		return
	fi

	_schema="$(tel_field schema_version)"
	if [ -z "${_schema}" ]; then
		# Distinguish "no schema field at all" from "not the contract format". A file written as
		# key=value parses as zero fields here, and saying "malformed" would send someone hunting
		# for corruption when the real answer is that a producer is speaking the wrong dialect.
		if grep -q '^schema_version=' "${FLUX_TELEMETRY_FILE}" 2>/dev/null; then
			st_fail "Telemetry uses key=value, not the v2 format"
			st_note "expected: schema_version <value>"
		elif [ "${_tel_format}" != "space" ]; then
			# Not the contract dialect at all — say which one, so the producer can be identified
			# without another device round trip.
			st_fail "Telemetry is ${_tel_format}, not the v2 format"
			st_note "expected: schema_version <value>"
		else
			st_fail "Telemetry snapshot has no schema_version"
		fi
		_tel_state="noschema"
		return
	fi
	case "${_schema}" in
	'' | *[!0-9]*)
		st_fail "Telemetry schema malformed (${_schema})"
		_tel_state="malformed"
		return
		;;
	esac
	if [ "${_schema}" -lt "${FLUX_TELEMETRY_SCHEMA}" ]; then
		# A legacy producer, named as such: the fix is to update SynthesisCore, not to reflash.
		st_fail "Telemetry schema v${_schema} is legacy"
		st_note "this build speaks v${FLUX_TELEMETRY_SCHEMA}"
		_tel_state="legacy"
		return
	fi
	if [ "${_schema}" -ne "${FLUX_TELEMETRY_SCHEMA}" ]; then
		st_fail "Telemetry schema v${_schema} unsupported"
		st_note "this build speaks v${FLUX_TELEMETRY_SCHEMA}"
		_tel_state="unsupported"
		return
	fi
	st_pass "Telemetry schema v${FLUX_TELEMETRY_SCHEMA}"
	_tel_state="ok"

	# Boot identity. The snapshot names the daemon that produced it; if that pid is not the fluxd
	# running now, the snapshot predates a restart and its contents describe a previous process.
	# Reported, never acted on.
	_tel_pid="$(tel_field daemon_pid)"
	_live_pid=""
	if command -v pidof >/dev/null 2>&1; then
		_live_pid="$(probe pidof fluxd | cut -d" " -f1)"
	fi
	if [ -z "${_tel_pid}" ] || [ -z "${_live_pid}" ]; then
		_tel_boot="unknown"
	elif [ "${_tel_pid}" = "${_live_pid}" ]; then
		_tel_boot="match"
	else
		_tel_boot="stale-producer"
		st_warn "Telemetry from a previous daemon"
	fi

	# Freshness. Both timestamps come from the same clock, so a negative age means the file is
	# stamped in the future — a clock jump, not a fresh snapshot, and it is not treated as one.
	_now="$(probe date +%s)"
	_mtime=""
	if command -v stat >/dev/null 2>&1; then
		_mtime="$(probe stat -c %Y "${FLUX_TELEMETRY_FILE}")"
	fi
	if [ -z "${_now}" ] || [ -z "${_mtime}" ]; then
		st_warn "Telemetry freshness unknown"
		return
	fi
	case "${_mtime}" in
	'' | *[!0-9]*)
		st_warn "Telemetry freshness unknown"
		return
		;;
	esac
	_tel_age=$((_now - _mtime))
	if [ "${_tel_age}" -lt 0 ]; then
		st_warn "Telemetry timestamp is in the future"
	elif [ "${_tel_age}" -le "${FLUX_TELEMETRY_MAX_AGE}" ]; then
		st_pass "SynthesisCore telemetry (${_tel_age}s old)"
	else
		st_warn "Telemetry stale (${_tel_age}s old)"
	fi
}

# ── 4. Runtime health ────────────────────────────────────────────────────────
check_health() {
	_profile_file="${FLUX_CONFIG_DIR}/current_profile"
	if [ ! -f "${_profile_file}" ]; then
		st_warn "No verified profile published yet"
	else
		_profile="$(head -1 "${_profile_file}" 2>/dev/null)"
		case "${_profile}" in
		'' | *[!0-9]*)
			st_fail "Verified profile unreadable"
			;;
		0 | 1 | 2 | 3 | 4)
			# This file is the VERIFIED profile, never the requested one — the status publisher
			# writes profile_mode_from_target(verified_profile, has_verified_profile), so an ask
			# that never took reports the safe default rather than the ask. That distinction is
			# the whole point of the field, so it is stated here rather than assumed.
			st_pass "Verified profile readable"
			;;
		*)
			st_fail "Verified profile out of range (${_profile})"
			;;
		esac
	fi

	# Degraded / rollback-failed / external-mutation / capability-limited state.
	#
	# These live in RuntimeProfileState and are NOT exported to disk by this runtime version —
	# docs/status-contract.md records that current_profile is the single published field, and
	# names runtime_status.json as the seam a future Diagnostics Channel would add. So the honest
	# result is "cannot be checked here", not a PASS. Reporting PASS would assert that rollback
	# succeeded and nothing was mutated externally, on no evidence at all.
	_status_file="${FLUX_CONFIG_DIR}/runtime_status.json"
	if [ ! -f "${_status_file}" ]; then
		st_warn "Degraded/rollback state not exported"
		st_note "this runtime publishes only current_profile"
		return
	fi

	# If a future runtime does publish it, read it — read-only, same rules.
	_degraded="$(prop_value degraded "${_status_file}")"
	_rollback="$(prop_value rollback_failed "${_status_file}")"
	_mutation="$(prop_value external_mutation "${_status_file}")"
	_limited="$(prop_value capability_limited "${_status_file}")"

	[ "${_rollback}" = "true" ] && st_fail "Rollback failed"
	[ "${_mutation}" = "true" ] && st_warn "External mutation detected"
	[ "${_degraded}" = "true" ] && st_warn "Runtime degraded"
	[ "${_limited}" = "true" ] && st_warn "Capability-limited"
	if [ "${_rollback}" != "true" ] && [ "${_mutation}" != "true" ] &&
		[ "${_degraded}" != "true" ] && [ "${_limited}" != "true" ]; then
		st_pass "Runtime health nominal"
	fi
}

# ── 5. Device capability ─────────────────────────────────────────────────────
check_capability() {
	_soc_file="${FLUX_CONFIG_DIR}/soc_recognition"
	if [ -f "${_soc_file}" ]; then
		_soc="$(head -1 "${_soc_file}" 2>/dev/null)"
		if [ -n "${_soc}" ] && [ "${_soc}" != "unknown" ]; then
			st_pass "SoC family recognised (${_soc})"
		else
			# Not a failure. An unidentified SoC means generic behaviour, which is a supported
			# configuration and the one every unvalidated device runs in.
			st_warn "SoC family not identified (generic)"
		fi
	else
		st_warn "SoC family record missing"
	fi

	# Generic capability is always available — it is the floor Flux falls back to, and it is what
	# makes an unrecognised device a limitation rather than a failure.
	st_pass "Generic capabilities available"

	# Vendor capability is gated behind physical-device validation. PhysicalDeviceRequired is a
	# WARN and never a FAIL: nothing is broken, the tuning is withheld ON PURPOSE until the
	# hardware is certified, and an unsupported capability produces zero writes by construction —
	# the execution engine plans nothing for a capability it has not validated, so there is no
	# write path to disable.
	st_warn "Vendor capabilities require device validation"
	st_note "gated tuning performs no writes"
}

# ── 6. WebUI and package assets ──────────────────────────────────────────────
check_assets() {
	if [ -f "${FLUX_MODULE_DIR}/webroot/index.html" ]; then
		st_pass "WebUI entry point"
	else
		st_fail "WebUI entry point missing"
	fi

	# Each branding key in module.prop must resolve to a file that is actually in the installed
	# module. A manager that cannot resolve one renders a broken card, and nothing else notices.
	_prop="${FLUX_MODULE_DIR}/module.prop"
	_broken=""
	for _key in banner webuiIcon actionIcon donateIcon; do
		_val="$(prop_value "${_key}" "${_prop}")"
		[ -z "${_val}" ] && continue
		[ -f "${FLUX_MODULE_DIR}/${_val}" ] || _broken="${_broken} ${_key}"
	done
	if [ -n "${_broken}" ]; then
		st_fail "Asset references unresolved:${_broken}"
	else
		st_pass "WebUI assets"
	fi
}

# ── 7. Safety ────────────────────────────────────────────────────────────────
check_safety() {
	# The legacy profiler was removed in the V2 cutover. Its reappearance would mean a stale
	# install or a downgrade that reintroduced the shell-based apply path this module retired.
	if [ -e "${FLUX_MODULE_DIR}/system/bin/flux_profiler" ] ||
		[ -e "${FLUX_MODULE_DIR}/system/bin/flux_profiler.sh" ]; then
		st_fail "Legacy profiler payload present"
	else
		st_pass "Legacy profiler absent"
	fi

	# Uninstall support is a safety property, not a convenience: without it a user has no
	# supported way to remove the module.
	if [ -f "${FLUX_MODULE_DIR}/uninstall.sh" ] && [ -f "${FLUX_MODULE_DIR}/cleanup.sh" ]; then
		st_pass "Rollback support"
	else
		st_fail "Uninstall/cleanup support missing"
	fi

	# No unrestricted shell endpoint. `eval` in a packaged script is the shape this would take —
	# a WebUI or an intent handing a string to the shell. Checked rather than assumed, because it
	# is the difference between a root module and a root exploit.
	_evals=""
	for _s in action.sh service.sh uninstall.sh cleanup.sh; do
		[ -f "${FLUX_MODULE_DIR}/${_s}" ] || continue
		# Anchored to a COMMAND position — start of line, or after a ; | & separator. Matching
		# "eval" anywhere would flag this script's own diagnostic string and every comment that
		# names the hazard, and a check that fires on the text describing it is a check that gets
		# deleted rather than fixed.
		if grep -qE '(^[[:space:]]*|[;|&][[:space:]]*)eval[[:space:]]' \
			"${FLUX_MODULE_DIR}/${_s}" 2>/dev/null; then
			_evals="${_evals} ${_s}"
		fi
	done
	if [ -n "${_evals}" ]; then
		st_fail "Shell eval endpoint present:${_evals}"
	else
		st_pass "No unrestricted shell endpoint"
	fi

	# A world-writable module or config directory would let any app on the device rewrite what
	# runs as root at boot.
	_writable=""
	for _d in "${FLUX_MODULE_DIR}" "${FLUX_CONFIG_DIR}"; do
		[ -d "${_d}" ] || continue
		_mode="$(probe stat -c %a "${_d}")"
		case "${_mode}" in
		*[2367]) _writable="${_writable} ${_d}" ;;
		esac
	done
	if [ -n "${_writable}" ]; then
		st_fail "World-writable path:${_writable}"
	else
		st_pass "No unsafe writable path"
	fi
}

# ── Run ──────────────────────────────────────────────────────────────────────
echo "Flux Self-Test"
echo "----------------------------"
echo ""

check_module
check_runtime
check_synthesiscore
check_health
check_capability
check_assets
check_safety

# ── Telemetry diagnostic ─────────────────────────────────────────────────────
# Deliberately narrow. This exists so a physical-device failure can be diagnosed from a
# screenshot, which means it must carry enough to identify a contract problem and nothing that
# would be unsafe to post in an issue.
#
# It prints ONLY: the path, the parser verdict, the detected schema version, the age, and whether
# the producing daemon is the one running now. It never prints the payload — that carries the
# focused package, pids, uids and thermal readings — and never prints a device identifier.
echo ""
echo "Telemetry diagnostic"
echo "  path:   ${FLUX_TELEMETRY_FILE}"
echo "  parser: ${_tel_state}"
echo "  format: ${_tel_format} (expected space), ${_tel_lines:-0} line(s)"
echo "  schema: ${_schema:-none} (expected ${FLUX_TELEMETRY_SCHEMA})"
if [ -n "${_tel_age}" ]; then
	echo "  age:    ${_tel_age}s (max ${FLUX_TELEMETRY_MAX_AGE}s)"
else
	echo "  age:    unknown"
fi
echo "  boot:   ${_tel_boot}"

echo ""
if [ "${_fail_count}" -gt 0 ]; then
	echo "Result: FAIL"
	echo ""
	echo "${_fail_count} critical check(s) failed."
	echo "Flux may not be running correctly."
	exit 1
fi
if [ "${_warn_count}" -gt 0 ]; then
	echo "Result: PASS WITH LIMITATIONS"
	echo ""
	echo "${_warn_count} item(s) are unavailable or"
	echo "device-validation-gated. This is expected"
	echo "on devices without certified vendor support."
	exit 2
fi
echo "Result: PASS"
exit 0
