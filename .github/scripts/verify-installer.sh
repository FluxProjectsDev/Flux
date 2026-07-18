#!/usr/bin/env bash
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
# Installer fixtures: the installer is actually EXECUTED here, not grepped.
#
# A source-level check can prove customize.sh contains a line. It cannot prove the install
# succeeds on APatch, that a corrupted payload aborts before printing a success line, or that a
# warning produces "installed with limitations" rather than "installed". Those are behaviours,
# they only exist at run time, and this is the one place they are observed.
#
# Method: a synthetic module ZIP built to the same layout compile_zip.sh produces (including the
# per-file .sha256 digests the installer verifies against), plus a fake module-manager
# environment providing ui_print / abort / set_perm / set_perm_recursive and the manager
# variables. customize.sh then runs under a real POSIX shell, unmodified.
#
# Blast radius: every device path the installer writes to is redirected into a temp root through
# FLUX_MODULE_DIR / FLUX_CONFIG_DIR / FLUX_MANAGER_BIN_DIRS, which exist for exactly this reason
# and default to the real paths when unset. The final case asserts nothing outside the temp root
# was touched.
#
# Exit codes: 0 every fixture passed; 1 at least one failed.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}" || exit 1

red() { printf '\033[31m%s\033[0m\n' "$1"; }
green() { printf '\033[32m%s\033[0m\n' "$1"; }
info() { printf '\033[36m•\033[0m %s\n' "$1"; }
head2() { printf '\n\033[1m%s\033[0m\n' "$1"; }

FAILURES=0
fail() {
	red "FAIL: $1"
	FAILURES=$((FAILURES + 1))
}

ROOT="$(mktemp -d)"
# KEEP_FIXTURES=1 leaves the trees and install logs in place. A failing fixture is close to
# undebuggable without the transcript it produced, and in CI the run is gone by the time anyone
# looks; this is the seam that makes "reproduce it locally" a one-liner.
if [ -z "${KEEP_FIXTURES:-}" ]; then
	trap 'rm -rf "${ROOT}"' EXIT
fi
info "fixture root: ${ROOT}"

# ── Build a synthetic package ────────────────────────────────────────────────
# Mirrors the layout of the real ZIP. The payload contents are stubs — this exercises the
# installer's control flow and verification, not the daemon — but the *structure*, and above all
# the .sha256 sidecar for every file, is exactly what compile_zip.sh produces, because that
# contract is what the installer's integrity checks read.
build_package() {
	local out="$1"
	local stage
	stage="${ROOT}/stage-$(basename "${out}" .zip)"
	rm -rf "${stage}"
	mkdir -p "${stage}"

	cp -r module/installer "${stage}/installer"
	rm -f "${stage}/installer"/*.sha256
	cp module/customize.sh module/verify.sh module/service.sh module/uninstall.sh \
		module/cleanup.sh module/action.sh module/module.prop "${stage}/"
	mkdir -p "${stage}/META-INF/com/google/android"
	cp module/META-INF/com/google/android/update-binary \
		module/META-INF/com/google/android/updater-script \
		"${stage}/META-INF/com/google/android/"

	# The build stamps these; the template ships them blank.
	sed -i 's|^version=.*|version=1.0.0 (999-testfix-release)|' "${stage}/module.prop"
	sed -i 's|^versionCode=.*|versionCode=999|' "${stage}/module.prop"
	sed -i 's|^updateJson=.*|updateJson=https://github.com/FluxProjectsDev/Flux/releases/latest/download/update.json|' "${stage}/module.prop"
	sed -i 's|^support=.*|support=https://github.com/FluxProjectsDev/Flux/issues|' "${stage}/module.prop"

	cp module/assets/branding/banner.webp "${stage}/banner.webp"
	cp module/assets/icons/action.webp "${stage}/action.webp"
	cp module/assets/icons/donate.webp "${stage}/donate.webp"

	# A stub daemon that answers the two subcommands the installer invokes. `check_gamelist`
	# reports valid only when the file both exists and parses as the marker the stub writes, so
	# the malformed-config fixture below has something real to fail against.
	mkdir -p "${stage}/libs/arm64-v8a" "${stage}/libs/armeabi-v7a" "${stage}/system/bin"
	cat >"${stage}/libs/arm64-v8a/fluxd" <<'STUB'
#!/bin/sh
case "$1" in
setup_gamelist) printf '{"flux_gamelist":1}\n' >"$(dirname "$2")/gamelist.json"; exit 0 ;;
check_gamelist) grep -q 'flux_gamelist' "${FLUX_CONFIG_DIR}/gamelist.json" 2>/dev/null; exit $? ;;
esac
exit 0
STUB
	chmod +x "${stage}/libs/arm64-v8a/fluxd"
	cp "${stage}/libs/arm64-v8a/fluxd" "${stage}/libs/armeabi-v7a/fluxd"
	printf '#!/bin/sh\nexit 0\n' >"${stage}/system/bin/flux_utility"
	chmod +x "${stage}/system/bin/flux_utility"

	printf 'PK-stub-synthesiscore\n' >"${stage}/synthesiscore.apk"
	printf 'com.example.game\n' >"${stage}/gamelist.txt"
	printf 'LICENSE\n' >"${stage}/LICENSE"
	printf 'NOTICE\n' >"${stage}/NOTICE.md"

	mkdir -p "${stage}/webroot"
	printf '<!doctype html><title>Flux</title>\n' >"${stage}/webroot/index.html"
	cp module/assets/icons/action.webp "${stage}/webroot/icon.webp"

	mkdir -p "${stage}/config"
	printf '{"schema":2}\n' >"${stage}/config/device_mitigation.json"

	# The digest sidecars, same rule as gen_sha256sum.sh.
	find "${stage}" -type f ! -name '*.sha256' -print0 |
		while IFS= read -r -d '' f; do
			sha256sum "${f}" | awk '{print $1}' >"${f}.sha256"
		done

	(cd "${stage}" && zip -qr9 "${out}" .)
}

# ── Fake module-manager environment ──────────────────────────────────────────
# Written to a file and sourced by the installer shell so customize.sh runs exactly as shipped.
write_env() {
	cat >"${ROOT}/fakeenv.sh" <<'ENVEOF'
ui_print() { echo "$1"; }
abort() { [ -n "${1:-}" ] && echo "$1"; exit 9; }
set_perm() { chown "$2:$3" "$1" 2>/dev/null; chmod "$4" "$1" 2>/dev/null; return 0; }
set_perm_recursive() {
	chmod -R "$4" "$1" 2>/dev/null
	find "$1" -type f -exec chmod "$5" {} + 2>/dev/null
	return 0
}
getprop() { echo ""; }
ENVEOF
}

# run_install <name> <zip> <manager-env-assignments...>
# Runs customize.sh in an isolated temp root. Echoes the log path; returns the installer's status.
run_install() {
	local name="$1" zip="$2"
	shift 2
	local case_root="${ROOT}/case-${name}"
	# MODPATH and FLUX_MODULE_DIR are deliberately DIFFERENT directories, because they are
	# different on a device: the manager stages the new module in modules_update/flux (MODPATH)
	# and the currently-installed one lives in modules/flux. Collapsing them in the fixture would
	# make a clean install look like a half-finished one.
	mkdir -p "${case_root}/modpath" "${case_root}/tmp" "${case_root}/config" \
		"${case_root}/ksu/bin" "${case_root}/ap/bin"

	local log="${case_root}/install.log"
	(
		export MODPATH="${case_root}/modpath"
		export TMPDIR="${case_root}/tmp"
		export ZIPFILE="${zip}"
		export FLUX_MODULE_DIR="${case_root}/installed"
		export FLUX_CONFIG_DIR="${case_root}/config"
		export FLUX_MANAGER_BIN_DIRS="${case_root}/ap/bin ${case_root}/ksu/bin"
		export ARCH="arm64" API="33"
		# The installer picks ASCII unless a UTF-8 locale is *declared*, which is the recovery
		# default. The host's own locale must not leak in and silently flip that decision — the
		# ASCII assertions below would then be testing the developer's terminal, not the code.
		unset LANG LC_ALL LC_CTYPE
		# shellcheck disable=SC2163
		for assign in "$@"; do export "${assign?}"; done
		# shellcheck disable=SC1091
		. "${ROOT}/fakeenv.sh"
		# shellcheck disable=SC1091
		. "${REPO_ROOT}/module/customize.sh"
	) >"${log}" 2>&1
	local rc=$?
	echo "${log}"
	return ${rc}
}

case_root_of() { echo "${ROOT}/case-$1"; }

write_env
PKG="${ROOT}/flux-test.zip"
build_package "${PKG}"
info "synthetic package: $(du -h "${PKG}" | cut -f1), $(unzip -l "${PKG}" | tail -1 | awk '{print $2}') entries"

# ═══ 1. Clean install on each manager ════════════════════════════════════════
head2 "1. Clean install: Magisk, KernelSU, APatch, unknown manager"

# check_clean_install <name> <log> <rc> <expected-manager> <expected-summary>
# expected-summary: SUCCESS (unqualified) or LIMITED. A recognised manager must produce an
# unqualified success — if a clean install on Magisk ever starts reporting limitations, that is a
# regression and this asserts it loudly rather than accepting either outcome.
check_clean_install() {
	local name="$1" log="$2" rc="$3" expect_manager="$4" expect_summary="${5:-SUCCESS}"
	local cr
	cr="$(case_root_of "${name}")"

	if [ "${rc}" -ne 0 ]; then
		fail "${name}: installer exited ${rc}"
		sed 's/^/    /' "${log}" | tail -25
		return
	fi
	if [ "${expect_summary}" = "LIMITED" ]; then
		grep -q "installed with limitations" "${log}" ||
			fail "${name}: expected SUCCESS WITH LIMITATIONS, got: $(grep -m1 'Flux installed' "${log}")"
	else
		if ! grep -q "Flux installed successfully" "${log}"; then
			fail "${name}: expected unqualified success, got: $(grep -m1 'Flux installed' "${log}")"
			# Print the warnings that caused the downgrade. Without this the failure says only
			# that the summary was wrong, which is the least useful half of the information —
			# the interesting part is always WHICH optional item was unavailable, and that
			# differs between a developer machine and a CI runner.
			printf '    warnings that downgraded it:\n' >&2
			grep -E '\[WARN\]|⚠' "${log}" | sed 's/^/      /' >&2
		fi
	fi
	grep -q "Manager: ${expect_manager}" "${log}" ||
		fail "${name}: expected 'Manager: ${expect_manager}', got: $(grep -m1 'Manager:' "${log}")"
	grep -q "\[8/8\]" "${log}" || fail "${name}: stage 8 never ran"

	for f in system/bin/fluxd system/bin/flux_utility service.sh uninstall.sh cleanup.sh \
		action.sh synthesiscore.apk webroot/index.html module.prop module.prop.orig; do
		[ -s "${cr}/modpath/${f}" ] || fail "${name}: installed tree is missing ${f}"
	done
	[ -x "${cr}/modpath/system/bin/fluxd" ] || fail "${name}: installed fluxd is not executable"
	[ -s "${cr}/config/gamelist.json" ] || fail "${name}: gamelist.json was not created"
	[ -s "${cr}/config/soc_recognition" ] || fail "${name}: soc_recognition was not written"
	# Digest sidecars are install-time metadata and must not survive into the module.
	if find "${cr}/modpath" -name '*.sha256' | grep -q .; then
		fail "${name}: .sha256 sidecars were left in the installed module"
	fi
}

LOG="$(run_install magisk "${PKG}" "MAGISK_VER_CODE=27000" "MAGISK_VER=27.0")" && RC=0 || RC=$?
check_clean_install magisk "${LOG}" "${RC}" "Magisk"
[ -f "$(case_root_of magisk)/modpath/skip_mount" ] &&
	fail "magisk: skip_mount must not be created for Magisk (it needs the mount)"
green "  Magisk clean install"

LOG="$(run_install ksu "${PKG}" "KSU=true" "KSU_VER=v0.9.5" "KSU_VER_CODE=11986")" && RC=0 || RC=$?
check_clean_install ksu "${LOG}" "${RC}" "KernelSU"
KSU_ROOT="$(case_root_of ksu)"
[ -f "${KSU_ROOT}/modpath/skip_mount" ] || fail "ksu: skip_mount was not created"
[ -L "${KSU_ROOT}/ksu/bin/fluxd" ] || fail "ksu: fluxd was not symlinked onto PATH"
[ -L "${KSU_ROOT}/ksu/bin/flux_utility" ] || fail "ksu: flux_utility was not symlinked"
green "  KernelSU clean install"

# APatch also sets KSU=true. If detection tested KSU first, this would report KernelSU.
LOG="$(run_install apatch "${PKG}" "KSU=true" "APATCH=true" "APATCH_VER_CODE=10672")" && RC=0 || RC=$?
check_clean_install apatch "${LOG}" "${RC}" "APatch"
[ -f "$(case_root_of apatch)/modpath/skip_mount" ] || fail "apatch: skip_mount was not created"
green "  APatch clean install (not misreported as KernelSU despite KSU=true)"

# SoC identification must never downgrade the summary, whichever way it resolves. This host may
# or may not match a family — an ARM dev machine usually does, an x86 CI runner does not — so the
# assertion is on the *severity*, not the outcome: it is informational in both branches, because
# an unidentified family means the runtime uses the generic behavior the summary already
# promises, not that anything the user installed is missing.
for mgr in magisk ksu apatch; do
	SOC_LINE="$(grep -i 'SoC family' "$(case_root_of "${mgr}")/install.log" || true)"
	if [ -z "${SOC_LINE}" ]; then
		fail "${mgr}: the installer reported nothing about SoC identification"
	elif grep -qE '\[WARN\]|⚠' <<<"${SOC_LINE}"; then
		fail "${mgr}: SoC identification produced a WARNING, which downgrades a healthy install:"
		fail "  ${SOC_LINE}"
	fi
done
green "  SoC identification is informational and never downgrades the summary"

LOG="$(run_install unknown "${PKG}")" && RC=0 || RC=$?
# An unrecognised manager is a genuine limitation, so the summary must be downgraded.
check_clean_install unknown "${LOG}" "${RC}" "unknown" LIMITED
grep -q "Unrecognised module manager" "${LOG}" ||
	fail "unknown: the unrecognised-manager warning was not shown"
green "  Unknown but contract-complete manager proceeds, with a warning"

# ═══ 2. Upgrade ══════════════════════════════════════════════════════════════
head2 "2. Upgrade: configuration preserved, legacy artifacts removed"

UP="$(case_root_of upgrade)"
mkdir -p "${UP}/installed" "${UP}/config" "${UP}/ksu/bin" "${UP}/ap/bin" "${UP}/tmp"
# A previous install: module.prop present (so mode == upgrade), a customised config, and the
# pre-V2 flux_profiler symlink that lives OUTSIDE the module directory.
printf 'id=flux\nname=Flux\nversion=old\nversionCode=1\n' >"${UP}/installed/module.prop"
printf '{"flux_gamelist":1,"user_added":"com.my.game"}\n' >"${UP}/config/gamelist.json"
printf 'user setting\n' >"${UP}/config/my_settings.conf"
ln -sf /data/adb/modules/flux/system/bin/flux_profiler "${UP}/ksu/bin/flux_profiler"

LOG="$(run_install upgrade "${PKG}" "KSU=true" "KSU_VER=v0.9.5")" && RC=0 || RC=$?
if [ "${RC}" -ne 0 ]; then
	fail "upgrade: installer exited ${RC}"
	sed 's/^/    /' "${LOG}" | tail -25
else
	grep -q "Mode: upgrade" "${LOG}" || fail "upgrade: not detected as an upgrade"
	grep -q "configuration is valid and was preserved" "${LOG}" ||
		fail "upgrade: valid configuration was not reported as preserved"
	grep -q "user_added" "${UP}/config/gamelist.json" ||
		fail "upgrade: the user's game list was overwritten"
	[ -f "${UP}/config/my_settings.conf" ] ||
		fail "upgrade: an unrelated user config file was destroyed"
	if [ -e "${UP}/ksu/bin/flux_profiler" ] || [ -L "${UP}/ksu/bin/flux_profiler" ]; then
		fail "upgrade: the stale pre-V2 flux_profiler symlink was left behind"
	fi
	grep -q "stale artifact" "${LOG}" || fail "upgrade: stale-artifact removal was not reported"
	green "  Upgrade preserves user configuration and clears the pre-V2 symlink"
fi

# ═══ 3. Malformed configuration ══════════════════════════════════════════════
head2 "3. Malformed configuration is backed up, not silently used"

MC="$(case_root_of malformed)"
mkdir -p "${MC}/installed" "${MC}/config" "${MC}/ksu/bin" "${MC}/ap/bin" "${MC}/tmp"
printf 'id=flux\nname=Flux\nversion=old\nversionCode=1\n' >"${MC}/installed/module.prop"
printf 'this is not the expected content at all\n' >"${MC}/config/gamelist.json"

LOG="$(run_install malformed "${PKG}" "KSU=true")" && RC=0 || RC=$?
if [ "${RC}" -ne 0 ]; then
	fail "malformed: installer exited ${RC}; a bad config must be recoverable"
	sed 's/^/    /' "${LOG}" | tail -25
else
	grep -q "malformed" "${LOG}" || fail "malformed: the condition was not reported"
	[ -s "${MC}/config/gamelist.json.invalid" ] ||
		fail "malformed: the unreadable configuration was not backed up"
	grep -q "flux_gamelist" "${MC}/config/gamelist.json" ||
		fail "malformed: defaults were not regenerated"
	# A warning must downgrade the summary, not be swallowed.
	grep -q "installed with limitations" "${LOG}" ||
		fail "malformed: a warning did not produce SUCCESS WITH LIMITATIONS"
	grep -q "Flux installed successfully" "${LOG}" &&
		fail "malformed: reported unqualified success despite a warning"
	green "  Malformed configuration backed up, defaults restored, summary downgraded"
fi

# ═══ 4. Fatal conditions abort and never print success ═══════════════════════
head2 "4. Fatal conditions"

# assert_fatal <name> <zip> <expected-message-fragment>
assert_fatal() {
	local name="$1" zip="$2" want="$3"
	shift 3
	local log rc
	log="$(run_install "${name}" "${zip}" "$@")" && rc=0 || rc=$?

	if [ "${rc}" -eq 0 ]; then
		fail "${name}: the installer succeeded when it should have aborted"
		return
	fi
	# The load-bearing assertion of this whole file.
	if grep -q "Flux installed successfully" "${log}"; then
		fail "${name}: a FATAL failure still printed a success line"
	fi
	if grep -q "installed with limitations" "${log}"; then
		fail "${name}: a FATAL failure printed a success-with-limitations line"
	fi
	grep -qi "${want}" "${log}" ||
		fail "${name}: expected a message matching '${want}', got: $(grep -m2 '\[FAIL\]' "${log}")"
	green "  ${name}: aborted, no success line"
}

assert_fatal unsupported-abi "${PKG}" "Unsupported CPU architecture" "ARCH=riscv64"
assert_fatal old-android "${PKG}" "not supported" "API=26"

# Corrupted checksum: rewrite a critical payload without updating its digest.
CORRUPT="${ROOT}/flux-corrupt.zip"
cp "${PKG}" "${CORRUPT}"
CTMP="${ROOT}/corrupt-work"
mkdir -p "${CTMP}"
printf 'tampered payload\n' >"${CTMP}/synthesiscore.apk"
(cd "${CTMP}" && zip -q "${CORRUPT}" synthesiscore.apk)
assert_fatal corrupt-checksum "${CORRUPT}" "Checksum mismatch" "KSU=true"

# Missing critical payload.
MISSING="${ROOT}/flux-missing.zip"
cp "${PKG}" "${MISSING}"
zip -qd "${MISSING}" synthesiscore.apk synthesiscore.apk.sha256
assert_fatal missing-payload "${MISSING}" "missing a required file" "KSU=true"

# Missing WebUI entry point.
NOWEB="${ROOT}/flux-noweb.zip"
cp "${PKG}" "${NOWEB}"
zip -qd "${NOWEB}" 'webroot/index.html' 'webroot/index.html.sha256'
assert_fatal missing-webui "${NOWEB}" "WebUI failed to install" "KSU=true"

# A tampered installer helper must be refused by the trust root before it is sourced.
TAMPER="${ROOT}/flux-tampered-helper.zip"
cp "${PKG}" "${TAMPER}"
TTMP="${ROOT}/tamper-work/installer"
mkdir -p "${TTMP}"
printf '#!/system/bin/sh\n# tampered\nflux_ui_init() { :; }\n' >"${TTMP}/ui.sh"
(cd "${ROOT}/tamper-work" && zip -q "${TAMPER}" installer/ui.sh)
assert_fatal tampered-helper "${TAMPER}" "Checksum mismatch" "KSU=true"

# Malformed module.prop in the package.
BADPROP="${ROOT}/flux-badprop.zip"
cp "${PKG}" "${BADPROP}"
BTMP="${ROOT}/badprop-work"
mkdir -p "${BTMP}"
printf 'id=flux\nname=Flux\nversion=1.0.0\nversionCode=not-a-number\n' >"${BTMP}/module.prop"
sha256sum "${BTMP}/module.prop" | awk '{print $1}' >"${BTMP}/module.prop.sha256"
(cd "${BTMP}" && zip -q "${BADPROP}" module.prop module.prop.sha256)
assert_fatal bad-versioncode "${BADPROP}" "versionCode is not numeric" "KSU=true"

# ═══ 5. Output contract ══════════════════════════════════════════════════════
head2 "5. Output contract"

REF_LOG="$(case_root_of magisk)/install.log"

# ASCII fallback is the default: no locale is set in the fixture environment.
if grep -qP '[^\x00-\x7F]' "${REF_LOG}"; then
	fail "the default (no-locale) install emitted non-ASCII bytes"
else
	green "  ASCII-only output when no UTF-8 locale is declared"
fi
grep -q '\[OK\]' "${REF_LOG}" || fail "no [OK] markers in ASCII mode"

# Unicode only on positive evidence.
LOG="$(run_install utf8 "${PKG}" "KSU=true" "LANG=en_US.UTF-8")" && RC=0 || RC=$?
[ "${RC}" -eq 0 ] || fail "utf8: installer exited ${RC}"
grep -q '✔' "${LOG}" || fail "utf8: Unicode markers were not used despite a UTF-8 locale"
green "  Unicode markers only when a UTF-8 locale is declared"

# No raw control characters other than newline and tab.
if LC_ALL=C grep -qP '[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]' "${REF_LOG}"; then
	fail "the installer emitted raw control characters into the log"
else
	green "  no escape sequences or raw control characters in the transcript"
fi

# No decorative delays anywhere in the installer.
SLEEPS="$(grep -rn '^[^#]*\bsleep\b' module/customize.sh module/installer/ || true)"
if [ -n "${SLEEPS}" ]; then
	fail "the installer contains a sleep — progress must reflect real work:"
	printf '%s\n' "${SLEEPS}" >&2
else
	green "  no sleep-based loading anywhere in the installer"
fi

# Stages appear once each, in order.
STAGES="$(grep -o '^\[[0-9]/8\]' "${REF_LOG}" | tr -d '[]' | cut -d/ -f1 | tr '\n' ' ')"
if [ "${STAGES}" = "1 2 3 4 5 6 7 8 " ]; then
	green "  all 8 stages ran exactly once, in order"
else
	fail "unexpected stage sequence: '${STAGES}'"
fi

# The installer must not claim vendor tuning it has not performed.
if grep -qiE 'implementing tweaks|applying (tweaks|optimi)' "${REF_LOG}"; then
	fail "the installer claims vendor tuning during flash"
else
	green "  no vendor optimisation is claimed during installation"
fi
grep -q "validation-gated" "${REF_LOG}" ||
	fail "the summary does not state that vendor capabilities remain validation-gated"

# ═══ 6. Action button and Donate/Support ═════════════════════════════════════
head2 "6. Action button and Donate/Support"

# action.sh must SURVIVE on the two managers that actually have an Action button. customize.sh
# used to delete it on exactly those, which is the regression this pins.
for mgr in ksu apatch magisk unknown; do
	[ -s "$(case_root_of "${mgr}")/modpath/action.sh" ] ||
		fail "action.sh was not installed on ${mgr}"
done
green "  action.sh is installed on Magisk, KernelSU, APatch and an unknown manager"

# run_action <name> <config-donation-url> <env assignments...>
# Runs the shipped action.sh with a stubbed Activity Manager that records what it was asked to
# open, so the assertions are about the intent actually issued rather than about the script text.
run_action() {
	local name="$1" donate="$2"
	shift 2
	local ar="${ROOT}/action-${name}"
	rm -rf "${ar}"
	mkdir -p "${ar}/mod/installer" "${ar}/bin"

	cp module/action.sh "${ar}/mod/action.sh"
	sed "s|^OFFICIAL_DONATION_URL=.*|OFFICIAL_DONATION_URL=\"${donate}\"|" \
		module/installer/config.sh >"${ar}/mod/installer/config.sh"

	cat >"${ar}/bin/am" <<AMEOF
#!/bin/sh
printf '%s\n' "\$*" >>"${ar}/am.calls"
exit 0
AMEOF
	cat >"${ar}/bin/pm" <<'PMEOF'
#!/bin/sh
# No WebUI viewer is installed, so the Magisk path falls through to its release-page hint.
exit 1
PMEOF
	chmod +x "${ar}/bin/am" "${ar}/bin/pm"
	: >"${ar}/am.calls"

	(
		# A minimal PATH containing only the stubs plus the real coreutils this script uses.
		# Subshell-local is the intent, not an oversight: the stub `am` and `pm` must not leak
		# into the rest of this script, or a later fixture would silently exercise them.
		# shellcheck disable=SC2030
		export PATH="${ar}/bin:/usr/bin:/bin"
		for assign in "$@"; do export "${assign?}"; done
		sh "${ar}/mod/action.sh"
	) >"${ar}/out.log" 2>&1
	echo "${ar}"
}

DONATE_URL="https://example.org/flux-support"

# A. Donation configured, manager with a native WebUI -> Action opens Support.
AR="$(run_action ksu-configured "${DONATE_URL}" "KSU=true" "KSU_VER_CODE=11986")"
grep -qF "${DONATE_URL}" "${AR}/am.calls" ||
	fail "KernelSU with a configured donation URL did not open it (am calls: $(cat "${AR}/am.calls"))"
grep -qF "android.intent.action.VIEW" "${AR}/am.calls" ||
	fail "the donation link was not opened with an explicit VIEW intent"
green "  KernelSU + configured URL: Action opens Support via an explicit VIEW intent"

AR="$(run_action apatch-configured "${DONATE_URL}" "KSU=true" "APATCH=true")"
grep -qF "${DONATE_URL}" "${AR}/am.calls" ||
	fail "APatch with a configured donation URL did not open it"
green "  APatch + configured URL: Action opens Support (not misrouted as KernelSU)"

# B. No donation configured -> nothing is opened, and the script still succeeds.
AR="$(run_action ksu-unconfigured "" "KSU=true")"
if [ -s "${AR}/am.calls" ]; then
	fail "with no donation URL configured, action.sh still opened something: $(cat "${AR}/am.calls")"
else
	green "  No URL configured: nothing is opened at all"
fi
grep -qi "does not currently have an official donation page" "${AR}/out.log" ||
	fail "the unconfigured case does not tell the user plainly that there is no donation page"
grep -qF "https://github.com/FluxProjectsDev/Flux" "${AR}/out.log" ||
	fail "the unconfigured case does not offer the repository as the alternative"

# C. Magisk has no WebUI button, so Action must spend itself on the WebUI instead.
AR="$(run_action magisk "${DONATE_URL}" "MAGISKTMP=/sbin/.magisk")"
grep -qF "${DONATE_URL}" "${AR}/am.calls" &&
	fail "Magisk Action opened the donation page; it must open the WebUI"
grep -qi "webui" "${AR}/out.log" ||
	fail "Magisk Action did not attempt to open the WebUI"
green "  Magisk: Action opens the WebUI, which the card cannot otherwise reach"

# D. MMRL opens the WebUI from the card; the script must not fight it.
AR="$(run_action mmrl "${DONATE_URL}" "MMRL=1")"
if [ -s "${AR}/am.calls" ]; then
	fail "under MMRL, action.sh started an activity instead of deferring to the card"
else
	green "  MMRL: defers to the module card, starts nothing"
fi

# E. Activity Manager unavailable -> graceful, and the URL is still shown.
AR_NOAM="${ROOT}/action-noam"
rm -rf "${AR_NOAM}"
mkdir -p "${AR_NOAM}/mod/installer" "${AR_NOAM}/bin"
cp module/action.sh "${AR_NOAM}/mod/action.sh"
sed "s|^OFFICIAL_DONATION_URL=.*|OFFICIAL_DONATION_URL=\"${DONATE_URL}\"|" \
	module/installer/config.sh >"${AR_NOAM}/mod/installer/config.sh"
(
	# No `am` on PATH at all. Scoped to this subshell for the same reason as above.
	# shellcheck disable=SC2031
	export PATH="${AR_NOAM}/bin:/usr/bin:/bin"
	export KSU=true
	sh "${AR_NOAM}/mod/action.sh"
) >"${AR_NOAM}/out.log" 2>&1
NOAM_RC=$?
[ "${NOAM_RC}" -eq 0 ] || fail "action.sh exited ${NOAM_RC} when the Activity Manager was absent"
grep -qF "${DONATE_URL}" "${AR_NOAM}/out.log" ||
	fail "with no Activity Manager, the URL was not printed for the user to open manually"
green "  Activity Manager absent: exits cleanly and prints the URL instead"

# F. No arbitrary URL and no shell execution surface.
# Comment lines are stripped first. The file's own header states "No eval", and matching the
# raw text would flag that sentence — a check that fails on its own documentation trains people
# to ignore it.
ACTION_CODE="$(sed -E 's/#.*$//' module/action.sh)"
if grep -qE '\beval\b' <<<"${ACTION_CODE}"; then
	fail "action.sh uses eval"
fi
# Every URL in the script must be a literal https:// constant or a reference to the config file's
# variables. A URL assembled from anything else is the thing this check exists to prevent.
# shellcheck disable=SC2016  # the literal '$' is the pattern: we are matching shell source text
BAD_URLS="$(grep -oE '(-d|open_url) +"?\$[A-Za-z_]+' <<<"${ACTION_CODE}" |
	grep -vE 'OFFICIAL_DONATION_URL|FLUX_REPO_URL|\$1' || true)"
if [ -n "${BAD_URLS}" ]; then
	fail "action.sh opens a URL from an unexpected variable: ${BAD_URLS}"
else
	green "  no eval, and every destination is a project-controlled constant"
fi

# G. The committed configuration is coherent, in whichever state it is left.
COMMITTED_DONATE="$(sed -n 's/^OFFICIAL_DONATION_URL=//p' module/installer/config.sh | tr -d '"'"'"'"')"
WEBUI_DONATE="$(sed -n "s/^const OFFICIAL_DONATION_URL = '\(.*\)'$/\1/p" \
	webui/src/views/Home.vue | head -1)"

if [ -n "${COMMITTED_DONATE}" ]; then
	case "${COMMITTED_DONATE}" in
	https://*) green "  donation URL configured: ${COMMITTED_DONATE}" ;;
	*) fail "OFFICIAL_DONATION_URL is set to a non-https value: ${COMMITTED_DONATE}" ;;
	esac

	# The shell constant and the WebUI constant are two restatements of ONE destination — a Vue
	# page cannot source a shell file. If they disagree, the module card and the WebUI send users
	# to different addresses, and nothing else in the build would notice.
	if [ "${WEBUI_DONATE}" = "${COMMITTED_DONATE}" ]; then
		green "  the WebUI support button points at the same destination"
	else
		fail "the WebUI and the installer disagree about the donation destination:"
		fail "  module/installer/config.sh: ${COMMITTED_DONATE}"
		fail "  webui/src/views/Home.vue:   ${WEBUI_DONATE:-<not found>}"
	fi
else
	green "  OFFICIAL_DONATION_URL is unset, so no donate metadata or button is claimed"
fi

# A private-channel Telegram link can never be a public donation destination: t.me/c/<id>/
# addresses a channel by its internal id and resolves only for accounts already in it. Exactly
# such a link was the donation destination here for a long time, so this pins the specific
# mistake rather than trusting it to stay fixed.
#
# Scoped to the two donation constants on purpose. A repo-wide grep also catches
# webui/src/views/Settings.vue's `openTelegram`, which is a community link rather than a donation
# one and is equally broken for non-members — but the correct public handle is not known here,
# and a check nobody can make pass is a check that gets disabled. That one is reported to the
# maintainer instead of failing the build.
DONATE_DESTINATIONS="${COMMITTED_DONATE} ${WEBUI_DONATE}"
case "${DONATE_DESTINATIONS}" in
*t.me/c/*)
	fail "a donation destination is a private-channel t.me/c/ link, which fails for every"
	fail "  account that is not already a member of that channel"
	;;
*) green "  no donation destination is a private-channel Telegram link" ;;
esac

# ═══ 7. Blast radius ═════════════════════════════════════════════════════════
head2 "7. Blast radius"
if [ -e /data/adb/modules/flux ]; then
	fail "a real module directory exists on this host — the fixtures must never touch it"
else
	green "  no real /data/adb was involved"
fi

head2 "═══ Result ═══"
if [ "${FAILURES}" -ne 0 ]; then
	red "${FAILURES} installer fixture(s) failed."
	red "Installer behaviour: NOT PROVEN"
	exit 1
fi
green "Installer behaviour: PROVEN"
green "  - clean install succeeds on Magisk, KernelSU, APatch and an unknown manager"
green "  - APatch is not misreported as KernelSU despite KSU=true"
green "  - upgrade preserves user configuration and clears the pre-V2 flux_profiler symlink"
green "  - malformed configuration is backed up and downgrades the summary, not the install"
green "  - every fatal condition aborts without printing any success line"
green "  - ASCII by default, Unicode only on a declared UTF-8 locale, no control characters"
green "  - eight stages, in order, with no sleep-based progress anywhere"
