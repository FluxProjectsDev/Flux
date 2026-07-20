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
		# Subshell-scoped deliberately, same as the self-test fixtures below: each install case
		# must see only its own tree.
		# shellcheck disable=SC2030,SC2031
		export FLUX_MODULE_DIR="${case_root}/installed"
		# shellcheck disable=SC2030,SC2031
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
	grep -q "Mode: Flux upgrade" "${LOG}" || fail "upgrade: not detected as an upgrade"
	grep -q "Configuration preserved" "${LOG}" ||
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

# ═══ 6. Action button: the Flux self-test ════════════════════════════════════
head2 "6. Action button (Flux self-test)"

# action.sh must SURVIVE on the managers that have an Action button. customize.sh used to delete
# it on exactly those, which is the regression this pins.
for mgr in ksu apatch magisk unknown; do
	[ -s "$(case_root_of "${mgr}")/modpath/action.sh" ] ||
		fail "action.sh was not installed on ${mgr}"
done
green "  action.sh is installed on Magisk, KernelSU, APatch and an unknown manager"

# st_tree <name> — a healthy installed module, plus its config dir. Echoes "<mod> <cfg>".
# Every negative case below starts from this and breaks exactly one thing, so a failure names the
# thing that was broken rather than a difference between two hand-built trees.
st_tree() {
	local r="${ROOT}/selftest-$1"
	rm -rf "${r}"
	mkdir -p "${r}/mod/system/bin" "${r}/mod/webroot" "${r}/cfg"

	cp module/action.sh "${r}/mod/action.sh"
	cp module/service.sh module/uninstall.sh module/cleanup.sh "${r}/mod/"

	cat >"${r}/mod/module.prop" <<'PROPEOF'
id=flux
name=Flux
version=1.0.0 (999-testfix-release)
versionCode=999
banner=banner.webp
webuiIcon=webroot/icon.webp
actionIcon=action.webp
donateIcon=donate.webp
PROPEOF

	# A minimal but REAL ELF header: e_machine at offset 18 is b7 00, i.e. AArch64. The self-test
	# reads those two bytes rather than trusting the install path, so the fixture must carry them.
	printf '\177ELF\002\001\001\000\000\000\000\000\000\000\000\000\002\000\267\000' \
		>"${r}/mod/system/bin/fluxd"
	chmod +x "${r}/mod/system/bin/fluxd"

	printf 'PK\003\004synthesiscore-stub' >"${r}/mod/synthesiscore.apk"
	printf '<!doctype html><title>Flux</title>\n' >"${r}/mod/webroot/index.html"
	for f in banner.webp action.webp donate.webp webroot/icon.webp; do
		printf 'stub' >"${r}/mod/${f}"
	done

	# The shared corpus, in the real wire format (key<SPACE>value). NOT hand-written here: the
	# previous fixture was key=value, which agreed with the parser bug instead of catching it,
	# and jni/tests/TelemetryContractTest.cpp holds this same file to the production decoder.
	cp .github/fixtures/telemetry/valid-v2.snapshot "${r}/cfg/synthesis_core.json"
	printf '3\n' >"${r}/cfg/current_profile"
	printf 'snapdragon\n' >"${r}/cfg/soc_recognition"

	chmod 755 "${r}/mod" "${r}/cfg"
	echo "${r}"
}

# st_run <tree-root> [env assignments...] — runs the self-test, captures output and exit code.
# ABI is pinned so the fixture does not inherit the host's (absent) ro.product.cpu.abi and quietly
# skip the ABI match on every case.
st_run() {
	local r="$1"
	shift
	(
		# Subshell-scoped ON PURPOSE: each fixture must see only its own tree, and a leaked
		# FLUX_MODULE_DIR would make a later case silently test the previous case's module.
		# shellcheck disable=SC2030,SC2031
		export FLUX_MODULE_DIR="${r}/mod"
		# shellcheck disable=SC2030,SC2031
		export FLUX_CONFIG_DIR="${r}/cfg"
		# shellcheck disable=SC2030,SC2031
		export PATH="${r}/bin:${PATH}"
		for assign in "$@"; do export "${assign?}"; done
		sh "${r}/mod/action.sh"
	) >"${r}/out.log" 2>&1
	echo $?
}

# A fake getprop so the ABI check has something deterministic to compare against, and a `pidof`
# that reports the daemon as absent unless a case says otherwise.
st_stub() {
	local r="$1" abi="$2" daemon="$3"
	mkdir -p "${r}/bin"
	cat >"${r}/bin/getprop" <<GPEOF
#!/bin/sh
[ "\$1" = "ro.product.cpu.abi" ] && echo "${abi}"
exit 0
GPEOF
	cat >"${r}/bin/pidof" <<PDEOF
#!/bin/sh
exit ${daemon}
PDEOF
	chmod +x "${r}/bin/getprop" "${r}/bin/pidof"
}

# ── A. Healthy installation ──────────────────────────────────────────────────
T="$(st_tree healthy)"
st_stub "${T}" arm64-v8a 0
RC="$(st_run "${T}")"
if grep -q '^\[FAIL\]' "${T}/out.log"; then
	fail "a healthy installation reported a FAIL:"
	grep '^\[FAIL\]' "${T}/out.log" | sed 's/^/    /' >&2
else
	green "  healthy install: no FAIL lines"
fi
grep -q "Flux daemon active" "${T}/out.log" ||
	fail "healthy install did not detect the running daemon"
grep -q "Telemetry schema v2" "${T}/out.log" ||
	fail "healthy install did not accept the schema-v2 snapshot"
# Vendor capability is gated on every device that is not certified, which today is all of them,
# so PASS WITH LIMITATIONS (exit 2) is the healthy outcome and a plain PASS is not yet reachable.
[ "${RC}" = "2" ] ||
	fail "healthy install exited ${RC}, expected 2 (PASS WITH LIMITATIONS)"
grep -q "^Result: PASS WITH LIMITATIONS" "${T}/out.log" ||
	fail "healthy install did not report PASS WITH LIMITATIONS"
green "  healthy install: PASS WITH LIMITATIONS, exit 2"

# ── B. The self-test WRITES NOTHING ──────────────────────────────────────────
# The whole point of the button. A manifest of every file's path, size, mode and content digest is
# taken before and after; anything created, deleted, resized, chmod-ed or rewritten shows up.
st_manifest() {
	find "$1" -type f -exec sha256sum {} + 2>/dev/null | sort
	find "$1" \( -type f -o -type d \) -printf '%p %m\n' 2>/dev/null | sort
}
T="$(st_tree readonly)"
st_stub "${T}" arm64-v8a 0
st_manifest "${T}/mod" >"${T}/before-mod.txt"
st_manifest "${T}/cfg" >"${T}/before-cfg.txt"
st_run "${T}" >/dev/null
st_manifest "${T}/mod" >"${T}/after-mod.txt"
st_manifest "${T}/cfg" >"${T}/after-cfg.txt"
if diff -q "${T}/before-mod.txt" "${T}/after-mod.txt" >/dev/null 2>&1 &&
	diff -q "${T}/before-cfg.txt" "${T}/after-cfg.txt" >/dev/null 2>&1; then
	green "  the self-test modified nothing (content, size and mode all unchanged)"
else
	fail "the self-test WROTE to the module or config tree:"
	diff "${T}/before-mod.txt" "${T}/after-mod.txt" | head -10 >&2
	diff "${T}/before-cfg.txt" "${T}/after-cfg.txt" | head -10 >&2
fi

# It must also never open a URL — the donation page now has its own manager button, and an Action
# that opened it would be the behaviour this change removed.
T="$(st_tree nourl)"
st_stub "${T}" arm64-v8a 0
mkdir -p "${T}/bin"
cat >"${T}/bin/am" <<AMEOF
#!/bin/sh
printf '%s\n' "\$*" >>"${T}/am.calls"
exit 0
AMEOF
chmod +x "${T}/bin/am"
: >"${T}/am.calls"
st_run "${T}" >/dev/null
if [ -s "${T}/am.calls" ]; then
	fail "the self-test issued an intent: $(cat "${T}/am.calls")"
else
	green "  no intent issued, no URL opened, nothing uploaded"
fi
grep -qF "sociabuzz.com" "${T}/out.log" &&
	fail "the self-test printed the donation URL; that button is the manager's \$ action now"

# ── C. Daemon inactive — a WARN, not a FAIL ──────────────────────────────────
# The state between flashing and rebooting. Telling a user their install FAILED here would send
# them to reflash over something a reboot fixes.
T="$(st_tree nodaemon)"
st_stub "${T}" arm64-v8a 1
RC="$(st_run "${T}")"
grep -q "^\[WARN\] Flux daemon not running" "${T}/out.log" ||
	fail "an inactive daemon was not reported as a WARN"
grep -q '^\[FAIL\]' "${T}/out.log" &&
	fail "an inactive daemon produced a FAIL; it must not be critical"
[ "${RC}" = "2" ] || fail "inactive daemon exited ${RC}, expected 2"
green "  daemon inactive: WARN, exit 2, no FAIL"

# ── D. Stale telemetry — a WARN ──────────────────────────────────────────────
T="$(st_tree stale)"
st_stub "${T}" arm64-v8a 0
touch -d '1 hour ago' "${T}/cfg/synthesis_core.json" 2>/dev/null ||
	touch -t 200001010000 "${T}/cfg/synthesis_core.json"
RC="$(st_run "${T}")"
grep -q "^\[WARN\] Telemetry stale" "${T}/out.log" ||
	fail "stale telemetry was not reported (out: $(grep -i telemetry "${T}/out.log" | tr '\n' ' '))"
[ "${RC}" = "2" ] || fail "stale telemetry exited ${RC}, expected 2"
green "  stale telemetry: WARN, exit 2"

# ── E. The telemetry contract, over the shared corpus ────────────────────────
# Every fixture here is also decoded by the production TelemetryDecoder in
# jni/tests/TelemetryContractTest.cpp. Two parsers, one corpus: if the wire format, the tokenizer
# or the schema bounds move, one of the two goes red. This is the check that would have caught
# the field failure, where the shell read key=value and every production consumer read
# key<SPACE>value.
#
# "<fixture>:<expected exit>:<expected parser state>"
for spec in \
	"valid-v2:2:ok" \
	"valid-v2-schema-last:2:ok" \
	"crlf:2:ok" \
	"missing-schema:1:noschema" \
	"keyvalue-not-contract:1:noschema" \
	"malformed-schema:1:malformed" \
	"unsupported-schema:1:unsupported" \
	"legacy-schema:1:legacy" \
	"duplicate-schema:1:duplicate" \
	"empty:1:empty" \
	"json-not-contract:1:noschema" \
	"tab-not-contract:1:noschema"; do
	fixture="${spec%%:*}"
	rest="${spec#*:}"
	want_rc="${rest%%:*}"
	want_state="${rest##*:}"

	T="$(st_tree "corpus-${fixture}")"
	st_stub "${T}" arm64-v8a 0
	cp ".github/fixtures/telemetry/${fixture}.snapshot" "${T}/cfg/synthesis_core.json"
	RC="$(st_run "${T}")"
	got_state="$(sed -n 's/^  parser: //p' "${T}/out.log" | head -1)"

	[ "${got_state}" = "${want_state}" ] ||
		fail "corpus ${fixture}: parser state '${got_state}', expected '${want_state}'"
	[ "${RC}" = "${want_rc}" ] ||
		fail "corpus ${fixture}: exit ${RC}, expected ${want_rc}"
done
green "  telemetry corpus: all 12 fixtures classified as the production decoder does"

# The dialect the producer is actually speaking is reported whatever the outcome, so a device that
# disagrees with the contract identifies itself in its own output instead of costing another round
# trip. Two devices hit the original bug and neither could be inspected from here.
for spec in "valid-v2:space" "keyvalue-not-contract:key=value" "json-not-contract:json" \
	"tab-not-contract:tab"; do
	fixture="${spec%%:*}"
	want="${spec##*:}"
	T="${ROOT}/selftest-corpus-${fixture}"
	got="$(sed -n 's/^  format: \([^ ]*\) .*/\1/p' "${T}/out.log" | head -1)"
	[ "${got}" = "${want}" ] ||
		fail "corpus ${fixture}: reported dialect '${got}', expected '${want}'"
done
green "  the diagnostic names the dialect it found (space / key=value / json / tab)"

# The regression, named explicitly so it cannot be quietly re-broken: a healthy schema-v2 snapshot
# must PASS, and a key=value file must NOT be mistaken for one.
T="${ROOT}/selftest-corpus-valid-v2"
grep -q "^\[PASS\] Telemetry schema v2" "${T}/out.log" ||
	fail "a healthy schema-v2 snapshot did not PASS (this was the physical-device failure)"
T="$(st_tree corpus-keyvalue-not-contract)"
st_stub "${T}" arm64-v8a 0
cp .github/fixtures/telemetry/keyvalue-not-contract.snapshot "${T}/cfg/synthesis_core.json"
st_run "${T}" >/dev/null
grep -q "not the v2 format" "${T}/out.log" ||
	fail "a key=value snapshot was not identified as the wrong dialect"

# Permission denied is its own state, distinct from absent and from malformed.
T="$(st_tree denied)"
st_stub "${T}" arm64-v8a 0
chmod 000 "${T}/cfg/synthesis_core.json"
RC="$(st_run "${T}")"
if [ "$(id -u)" = "0" ]; then
	info "  skipped: running as root, an unreadable file is still readable"
else
	grep -q "^\[FAIL\] Telemetry snapshot unreadable (permission denied)" "${T}/out.log" ||
		fail "an unreadable snapshot was not reported as permission denied"
	[ "${RC}" = "1" ] || fail "permission denied exited ${RC}, expected 1"
fi
chmod 644 "${T}/cfg/synthesis_core.json"

# A leftover atomic temp file is not the snapshot and must not be read as one. SynthesisCore
# writes temp -> fsync -> rename, so the target is never partial; a stray temp is debris.
T="$(st_tree atomictmp)"
st_stub "${T}" arm64-v8a 0
printf 'schema_version 9\n' >"${T}/cfg/synthesis_core.json.tmp"
RC="$(st_run "${T}")"
grep -q "^\[PASS\] Telemetry schema v2" "${T}/out.log" ||
	fail "a stray .tmp file changed the verdict for a healthy snapshot"
[ "${RC}" = "2" ] || fail "stray temp file exited ${RC}, expected 2"
green "  permission denied, and a stray atomic temp file, are handled as their own states"

# The diagnostic block reports safe metadata only — never the payload, which carries the focused
# package, pids and uids.
T="$(st_tree diag)"
st_stub "${T}" arm64-v8a 0
st_run "${T}" >/dev/null
for field in "path:" "parser:" "schema:" "age:" "boot:"; do
	grep -q "  ${field}" "${T}/out.log" ||
		fail "the telemetry diagnostic omits '${field}'"
done
grep -q "com.example.game" "${T}/out.log" &&
	fail "the diagnostic printed the focused package from the telemetry payload"
grep -qE "focused_uid|focused_pid|thermal_headroom" "${T}/out.log" &&
	fail "the diagnostic printed raw telemetry payload fields"
green "  diagnostic reports path/parser/schema/age/boot, and no payload"

# ── F. Missing SynthesisCore — critical ──────────────────────────────────────
T="$(st_tree noapk)"
st_stub "${T}" arm64-v8a 0
rm -f "${T}/mod/synthesiscore.apk"
RC="$(st_run "${T}")"
grep -q "^\[FAIL\] SynthesisCore payload missing" "${T}/out.log" ||
	fail "a missing SynthesisCore payload was not reported"
[ "${RC}" = "1" ] || fail "missing SynthesisCore exited ${RC}, expected 1"
# A truncated/placeholder APK is a different failure from an absent one, and is caught too.
T="$(st_tree badapk)"
st_stub "${T}" arm64-v8a 0
printf 'not-a-zip' >"${T}/mod/synthesiscore.apk"
RC="$(st_run "${T}")"
grep -q "^\[FAIL\] SynthesisCore payload is not an APK" "${T}/out.log" ||
	fail "a non-APK SynthesisCore payload was not reported"
green "  missing / non-APK SynthesisCore: FAIL, exit 1"

# ── G. Missing WebUI — critical ──────────────────────────────────────────────
T="$(st_tree nowebui)"
st_stub "${T}" arm64-v8a 0
rm -f "${T}/mod/webroot/index.html"
RC="$(st_run "${T}")"
grep -q "^\[FAIL\] WebUI entry point missing" "${T}/out.log" ||
	fail "a missing WebUI entry point was not reported"
[ "${RC}" = "1" ] || fail "missing WebUI exited ${RC}, expected 1"
# An unresolvable branding path is caught separately from a missing entry point.
T="$(st_tree noasset)"
st_stub "${T}" arm64-v8a 0
rm -f "${T}/mod/banner.webp"
RC="$(st_run "${T}")"
grep -q "^\[FAIL\] Asset references unresolved" "${T}/out.log" ||
	fail "an unresolvable module.prop asset reference was not reported"
green "  missing WebUI / unresolved asset: FAIL, exit 1"

# ── H. PhysicalDeviceRequired is a WARN, never a FAIL ────────────────────────
# The rule the whole capability model rests on: withheld vendor tuning is a deliberate safety
# decision, not a fault, and must never be presented to a user as a broken install.
T="$(st_tree gated)"
st_stub "${T}" arm64-v8a 0
st_run "${T}" >/dev/null
grep -q "^\[WARN\] Vendor capabilities require device validation" "${T}/out.log" ||
	fail "vendor capability gating was not reported as a WARN"
grep -qi "^\[FAIL\].*vendor" "${T}/out.log" &&
	fail "vendor capability gating produced a FAIL; it must be a WARN"
grep -q "gated tuning performs no writes" "${T}/out.log" ||
	fail "the self-test does not state that gated capabilities perform no writes"
# An unidentified SoC is the generic case, and is also a WARN rather than a failure.
T="$(st_tree genericsoc)"
st_stub "${T}" arm64-v8a 0
printf 'unknown\n' >"${T}/cfg/soc_recognition"
st_run "${T}" >/dev/null
grep -q "^\[WARN\] SoC family not identified" "${T}/out.log" ||
	fail "an unidentified SoC family was not reported as a WARN"
grep -q "^\[PASS\] Generic capabilities available" "${T}/out.log" ||
	fail "generic capability availability was not reported"
green "  PhysicalDeviceRequired and generic SoC: WARN, never FAIL"

# ── I. Rollback-failed state — critical when the runtime exports it ──────────
# This runtime publishes only current_profile, so the honest default is "not exported" rather than
# a PASS asserting a rollback succeeded on no evidence. When a future runtime does publish it, a
# failed rollback must be critical.
T="$(st_tree nostatus)"
st_stub "${T}" arm64-v8a 0
st_run "${T}" >/dev/null
grep -q "^\[WARN\] Degraded/rollback state not exported" "${T}/out.log" ||
	fail "the unexported-state case does not say so plainly"
grep -q "^\[PASS\] Runtime health nominal" "${T}/out.log" &&
	fail "runtime health was reported nominal without any evidence for it"

T="$(st_tree rollbackfail)"
st_stub "${T}" arm64-v8a 0
printf 'rollback_failed=true\ndegraded=true\n' >"${T}/cfg/runtime_status.json"
RC="$(st_run "${T}")"
grep -q "^\[FAIL\] Rollback failed" "${T}/out.log" ||
	fail "an exported rollback-failed state was not reported as critical"
[ "${RC}" = "1" ] || fail "rollback-failed exited ${RC}, expected 1"

T="$(st_tree mutation)"
st_stub "${T}" arm64-v8a 0
printf 'external_mutation=true\ncapability_limited=true\n' >"${T}/cfg/runtime_status.json"
RC="$(st_run "${T}")"
grep -q "^\[WARN\] External mutation detected" "${T}/out.log" ||
	fail "an exported external-mutation state was not surfaced"
grep -q "^\[WARN\] Capability-limited" "${T}/out.log" ||
	fail "an exported capability-limited state was not surfaced"
[ "${RC}" = "2" ] || fail "external mutation exited ${RC}, expected 2"
green "  rollback-failed FAIL; mutation and capability-limited WARN; unexported stated honestly"

# ── J. Legacy profiler unexpectedly present — critical ───────────────────────
for legacy in flux_profiler flux_profiler.sh; do
	T="$(st_tree "legacy-${legacy}")"
	st_stub "${T}" arm64-v8a 0
	printf '#!/system/bin/sh\n' >"${T}/mod/system/bin/${legacy}"
	RC="$(st_run "${T}")"
	grep -q "^\[FAIL\] Legacy profiler payload present" "${T}/out.log" ||
		fail "a reappeared ${legacy} was not reported"
	[ "${RC}" = "1" ] || fail "legacy profiler (${legacy}) exited ${RC}, expected 1"
done
green "  legacy profiler present: FAIL, exit 1"

# ── K. Unsupported ABI — critical ────────────────────────────────────────────
# A 32-bit fluxd on an arm64 device fails to exec at boot with nothing in the log to explain it.
T="$(st_tree badabi)"
st_stub "${T}" arm64-v8a 0
printf '\177ELF\001\001\001\000\000\000\000\000\000\000\000\000\002\000\050\000' \
	>"${T}/mod/system/bin/fluxd"
chmod +x "${T}/mod/system/bin/fluxd"
RC="$(st_run "${T}")"
grep -q "^\[FAIL\] Runtime ABI mismatch" "${T}/out.log" ||
	fail "an armeabi-v7a binary on an arm64 device was not reported"
[ "${RC}" = "1" ] || fail "ABI mismatch exited ${RC}, expected 1"
# The matching 32-bit case must still pass, or the check is just rejecting 32-bit devices.
T="$(st_tree abi32)"
st_stub "${T}" armeabi-v7a 0
printf '\177ELF\001\001\001\000\000\000\000\000\000\000\000\000\002\000\050\000' \
	>"${T}/mod/system/bin/fluxd"
chmod +x "${T}/mod/system/bin/fluxd"
st_run "${T}" >/dev/null
grep -q "^\[PASS\] Flux runtime binary (armeabi-v7a)" "${T}/out.log" ||
	fail "a correct armeabi-v7a install was not accepted"
# A non-executable or empty binary is a distinct, also-critical failure.
T="$(st_tree noexec)"
st_stub "${T}" arm64-v8a 0
chmod 644 "${T}/mod/system/bin/fluxd"
RC="$(st_run "${T}")"
grep -q "^\[FAIL\] Flux runtime binary is not executable" "${T}/out.log" ||
	fail "a non-executable fluxd was not reported"
[ "${RC}" = "1" ] || fail "non-executable fluxd exited ${RC}, expected 1"
green "  ABI mismatch / non-executable runtime: FAIL; matching 32-bit install: PASS"

# ── L. Command injection ─────────────────────────────────────────────────────
# Every value the self-test reads comes off the filesystem, and a hostile or corrupted file must
# be data, never code. A canary file proves it: if any injected payload ran, the canary appears.
CANARY="${ROOT}/selftest-canary"
rm -f "${CANARY}"
T="$(st_tree injection)"
st_stub "${T}" arm64-v8a 0
printf 'id=flux; touch %s\nname=Flux\nversionCode=999\n' "${CANARY}" >"${T}/mod/module.prop"
st_run "${T}" >/dev/null
[ -e "${CANARY}" ] && fail "a command injected through module.prop EXECUTED"

# shellcheck disable=SC2016  # single quotes are the point: these must stay UNEXPANDED payloads
for payload in '$(touch CANARY)' '`touch CANARY`' '; touch CANARY' '&& touch CANARY' '| touch CANARY'; do
	T="$(st_tree "inject-$(printf '%s' "${payload}" | tr -cd 'a-zA-Z')")"
	st_stub "${T}" arm64-v8a 0
	printf '%s\n' "$(printf '%s' "${payload}" | sed "s|CANARY|${CANARY}|")" \
		>"${T}/cfg/soc_recognition"
	printf "schema_version %s\n" "$(printf '%s' "${payload}" | sed "s|CANARY|${CANARY}|")" \
		>"${T}/cfg/synthesis_core.json"
	printf '%s\n' "$(printf '%s' "${payload}" | sed "s|CANARY|${CANARY}|")" \
		>"${T}/cfg/current_profile"
	st_run "${T}" >/dev/null
	[ -e "${CANARY}" ] && fail "payload executed via a config file: ${payload}"
done
# A filename-shaped payload in a module.prop asset value must not escape the path test either.
T="$(st_tree injectasset)"
st_stub "${T}" arm64-v8a 0
sed -i "s|^banner=.*|banner=../../../$(basename "${CANARY}")|" "${T}/mod/module.prop"
st_run "${T}" >/dev/null
[ -e "${CANARY}" ] && fail "an asset path payload executed"
grep -q "^\[FAIL\] Asset references unresolved" "${T}/out.log" ||
	fail "a traversing asset path was not reported as unresolved"
[ -e "${CANARY}" ] || green "  command injection: every payload treated as data, nothing executed"

# ── M. Android ash compatibility ─────────────────────────────────────────────
# Android's /system/bin/sh is mksh or toybox ash. dash is the closest stand-in available here.
# busybox ash is not installed on this host, which is a stated gap rather than a claim.
if command -v dash >/dev/null 2>&1; then
	T="$(st_tree ash)"
	st_stub "${T}" arm64-v8a 0
	(
		# Same scoping rationale as st_run above.
		# shellcheck disable=SC2030,SC2031
		export FLUX_MODULE_DIR="${T}/mod" FLUX_CONFIG_DIR="${T}/cfg"
		# shellcheck disable=SC2030,SC2031
		export PATH="${T}/bin:${PATH}"
		dash "${T}/mod/action.sh"
	) >"${T}/dash.log" 2>&1
	DASH_RC=$?
	[ "${DASH_RC}" = "2" ] ||
		fail "under dash the self-test exited ${DASH_RC}, expected 2"
	# Same bytes under dash as under sh: a bashism would show up as a diff or a syntax error.
	st_run "${T}" >/dev/null
	# The telemetry age advances between the two runs, so it is normalised out. Everything else
	# must match byte for byte — a bashism shows up as a diff or a syntax error, not as a clock.
	norm_clock() { sed -E -e 's/\([0-9]+s old\)/(Ns old)/' -e 's/^  age:    [0-9]+s/  age:    Ns/'; }
	norm_clock <"${T}/dash.log" >"${T}/dash.norm"
	norm_clock <"${T}/out.log" >"${T}/sh.norm"
	if diff -q "${T}/dash.norm" "${T}/sh.norm" >/dev/null 2>&1; then
		green "  identical output under dash and sh (no bashisms)"
	else
		fail "the self-test behaves differently under dash:"
		diff "${T}/dash.norm" "${T}/sh.norm" | head -10 >&2
	fi
	grep -qiE "not found|syntax error|bad substitution|unexpected" "${T}/dash.log" &&
		fail "dash reported a shell error running the self-test"
fi

# No bash-only syntax in the source either, independent of what the fixtures happen to exercise.
# Comments stripped first, and `[[` anchored as a command: [[:space:]] and [[:digit:]] are POSIX
# character classes that legitimately contain "[[", so an unanchored match flags correct code.
if printf '%s\n' "$(sed -E 's/#.*$//' module/action.sh)" |
	grep -nE '(^|[[:space:]]|;)\[\[[[:space:]]|^[[:space:]]*local[[:space:]]|<<<|declare |=\(' \
		>"${ROOT}/bashisms.txt" 2>/dev/null; then
	fail "action.sh contains bash-only syntax:"
	sed 's/^/    /' "${ROOT}/bashisms.txt" | head -10 >&2
else
	green "  no bash-only syntax, no 'local', no [[ ]], no herestrings"
fi

# ── N. No eval, no arbitrary command, no network ─────────────────────────────
# Comments are stripped first: the file's own header describes each hazard by name, and a check
# that fires on its own documentation trains people to ignore it.
ACTION_CODE="$(sed -E 's/#.*$//' module/action.sh)"
printf '%s\n' "${ACTION_CODE}" | grep -qE '(^[[:space:]]*|[;|&][[:space:]]*)eval[[:space:]]' &&
	fail "action.sh uses eval"
# Word-anchored with \b: a plain substring match for "nc" also hits "since" and "instance", and
# a negated bracket class does not behave portably across the greps this runs under.
for forbidden in curl wget nc ping 'am start' 'pm install' 'settings put'; do
	printf '%s\n' "${ACTION_CODE}" | grep -qE "\\b${forbidden}\\b" &&
		fail "action.sh performs a forbidden operation: ${forbidden}"
done
# Redirections that would write. The self-test may only read; `>` or `>>` onto a device path is
# the shape a write would take.
printf '%s\n' "${ACTION_CODE}" | grep -qE '>[[:space:]]*"?\$\{?(FLUX_MODULE_DIR|FLUX_CONFIG_DIR)' &&
	fail "action.sh redirects output into the module or config tree"
green "  no eval, no network tool, no intent, no write redirection"


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

# ═══ 7. Installer banner (golden output) ═════════════════════════════════════
head2 "7. Installer banner"

# Byte-for-byte against a committed golden file, per tier. The banner is a fixed reviewed
# constant: a single space edited in the heredoc changes the logo, and nothing else in the build
# would notice. Emitted through the real ui.sh so this tests what actually reaches the console,
# not the heredoc's source text.
banner_at() {
	local cols="$1"
	if [ "${cols}" -eq 0 ]; then
		sh -c '. module/installer/ui.sh; flux_print_banner'
	else
		COLUMNS="${cols}" sh -c '. module/installer/ui.sh; flux_print_banner'
	fi
}

banner_verbose() {
	FLUX_BANNER_VERBOSE=1 sh -c '. module/installer/ui.sh; flux_print_banner'
}

# The DEFAULT tier is what a device shows; the detailed one is now reachable only through
# FLUX_BANNER_VERBOSE=1 and is kept as the reference form. Both are pinned.
banner_verbose >"${ROOT}/banner-detailed.actual"
if diff -u .github/fixtures/banner-detailed.golden "${ROOT}/banner-detailed.actual" \
	>"${ROOT}/banner-detailed.diff" 2>&1; then
	green "  banner-detailed (FLUX_BANNER_VERBOSE=1): byte-identical to its golden fixture"
else
	fail "banner-detailed differs from .github/fixtures/banner-detailed.golden:"
	sed 's/^/    /' "${ROOT}/banner-detailed.diff" | head -30 >&2
fi

# The tall reference must NOT be what an ordinary flash prints. This is the regression that
# matters for readability: if the default ever falls back to the 36-line block again, stage 1
# drops below the fold on a phone and the first thing a user sees of an install is scrollback.
if diff -q .github/fixtures/banner-detailed.golden .github/fixtures/banner-default.golden \
	>/dev/null 2>&1; then
	fail "the default banner is the full-height reference; it must be the compact form"
else
	green "  the full-height reference is not the default output"
fi

for spec in "0:banner-default" "30:banner-compact" "20:banner-plain"; do
	cols="${spec%%:*}"
	name="${spec##*:}"
	golden=".github/fixtures/${name}.golden"
	actual="${ROOT}/${name}.actual"
	banner_at "${cols}" >"${actual}"
	if [ ! -f "${golden}" ]; then
		fail "missing golden fixture: ${golden}"
	elif diff -u "${golden}" "${actual}" >"${ROOT}/${name}.diff" 2>&1; then
		green "  ${name}: byte-identical to its golden fixture"
	else
		fail "${name} differs from ${golden}:"
		sed 's/^/    /' "${ROOT}/${name}.diff" | head -30 >&2
	fi
done

# Each tier's widest line must fit the width that selects it. A "fallback" that overflows the
# console it was chosen for has accomplished nothing — and this is easy to break, because the
# strapline is wider than the compact emblem.
for spec in "30:25" "20:16"; do
	cols="${spec%%:*}"
	maxw="${spec##*:}"
	got="$(banner_at "${cols}" | awk '{ if (length > m) m = length } END { print m+0 }')"
	if [ "${got}" -gt "${cols}" ]; then
		fail "at COLUMNS=${cols} the banner is ${got} columns wide and would wrap"
	elif [ "${got}" -ne "${maxw}" ]; then
		fail "at COLUMNS=${cols} expected widest ${maxw}, got ${got}"
	fi
done
green "  every tier fits the width that selects it (38 / 25 / 16)"

# The default tier's budget, in both axes. Width keeps it off the wrap boundary of a 40-column
# console; HEIGHT is the reason this change exists at all — the whole branding block has to leave
# room for stage 1 on the first screen of a mobile module-manager terminal.
DEFAULT_W="$(banner_at 0 | awk '{ if (length > m) m = length } END { print m+0 }')"
DEFAULT_H="$(banner_at 0 | wc -l | tr -d ' ')"
if [ "${DEFAULT_W}" -gt 40 ]; then
	fail "the default banner is ${DEFAULT_W} columns wide; the limit is 40"
elif [ "${DEFAULT_H}" -gt 24 ]; then
	fail "the default branding block is ${DEFAULT_H} lines; the limit is 24"
else
	green "  default branding block: ${DEFAULT_H} lines x ${DEFAULT_W} columns (limits 24 x 40)"
fi

# Tabs would be re-expanded to a different width by every console and destroy the alignment.
if grep -qP '\t' .github/fixtures/*.golden; then
	fail "the banner contains a tab; ASCII art must be spaces only"
else
	green "  no tabs in any banner tier"
fi
if grep -q $'\r' .github/fixtures/*.golden; then
	fail "the banner has CRLF line endings"
else
	green "  LF line endings"
fi

# Magisk's ui_print writes with `echo -e`, which interprets \a \b \c \e \f \n \r \t \v \\ and
# \0nnn. The art is full of backslashes; every one of them must be followed by something that is
# NOT an escape letter, or the logo silently loses characters on real devices while looking
# perfect in any fixture that echoes plainly.
if grep -qP '\\\\[abcefnrtv0\\\\]' .github/fixtures/banner-*.golden; then
	fail "a banner tier contains a backslash escape that 'echo -e' would consume"
	grep -nP '\\\\[abcefnrtv0\\\\]' .github/fixtures/banner-*.golden >&2
else
	green "  no backslash sequence that echo -e would interpret"
fi

# Prove it rather than trust the pattern: re-emit through an echo -e based ui_print, the way
# Magisk actually does, and require the content to be unchanged.
cat >"${ROOT}/magisk-uiprint.sh" <<'MAGISKEOF'
ui_print() { echo -e "$1"; }
. module/installer/ui.sh
flux_print_banner
MAGISKEOF
if command -v dash >/dev/null 2>&1; then
	dash "${ROOT}/magisk-uiprint.sh" 2>&1 | sed 's/^-e //' >"${ROOT}/echoe.actual"
	if diff -q .github/fixtures/banner-default.golden "${ROOT}/echoe.actual" >/dev/null 2>&1; then
		green "  survives an 'echo -e' ui_print unchanged (the Magisk path)"
	else
		fail "the banner is altered when ui_print uses 'echo -e', as Magisk's does"
		diff .github/fixtures/banner-default.golden "${ROOT}/echoe.actual" | head -20 >&2
	fi
fi

# Portability across the shells a manager might use. bash and dash are the two available here;
# dash is the closest stand-in for Android's ash/toybox sh.
for shell in dash bash; do
	command -v "${shell}" >/dev/null 2>&1 || continue
	"${shell}" -c '. module/installer/ui.sh; flux_print_banner' >"${ROOT}/${shell}.actual" 2>&1
	if diff -q .github/fixtures/banner-default.golden "${ROOT}/${shell}.actual" >/dev/null 2>&1; then
		green "  identical under ${shell}"
	else
		fail "the banner differs under ${shell}"
	fi
done

# The approved reference image is a design input, not module content.
if [ -n "$(find . -name 'logo_ascii*' -not -path './.git/*' -path './module/*' 2>/dev/null)" ]; then
	fail "the reference logo image is inside module/ and would be packaged"
else
	green "  the reference image is not inside module/"
fi

# The retired generic FIGlet wordmark must not come back.
if grep -qF '|____| \___/' module/installer/ui.sh 2>/dev/null; then
	fail "the retired generic FIGlet FLUX banner is still present in ui.sh"
else
	green "  the previous generic ASCII banner is gone"
fi

# The strapline the approved design calls for.
if grep -q "Adaptive Runtime Engine" .github/fixtures/banner-default.golden &&
	grep -qF "Hardware-aware | Verified | Reversible" .github/fixtures/banner-default.golden; then
	green "  strapline present: 'Adaptive Runtime Engine' / 'Hardware-aware | Verified | Reversible'"
else
	fail "the banner strapline is missing or altered"
fi

# ═══ 8. Blast radius ═════════════════════════════════════════════════════════
head2 "8. Blast radius"
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
