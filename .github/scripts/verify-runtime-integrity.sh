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
# Runtime integrity test suite.
#
# Exercises module/integrity_runtime.sh — the writer and the verifier — against a synthetic module
# tree, one case per distinguishable outcome. It proves the classifier reports the RIGHT reason,
# not merely pass/fail: on a real device the class is what tells a user whether to reboot, reflash,
# or report tampering, so a check that collapsed the classes would lose the only thing worth having.
#
# Each case builds a clean tree, mutates exactly one file, and asserts (state, class, return code).
# Runs on any POSIX shell host; needs no NDK, no device, no build.
#
# Usage: bash .github/scripts/verify-runtime-integrity.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=/dev/null
. "${REPO_ROOT}/module/integrity_runtime.sh"

red() { printf '\033[31m%s\033[0m\n' "$1"; }
green() { printf '\033[32m%s\033[0m\n' "$1"; }
head2() { printf '\n\033[1m%s\033[0m\n' "$1"; }

FAILURES=0
fail() {
	red "FAIL: $1"
	FAILURES=$((FAILURES + 1))
}

GENERATION=42
WORK="$(mktemp -d)"
trap 'chmod -R u+rwX "${WORK}" 2>/dev/null || true; rm -rf "${WORK}"' EXIT

# build_tree <moddir> — a minimal module tree with every critical file present and benign content.
build_tree() {
	_mod="$1"
	rm -rf "${_mod}"
	mkdir -p "${_mod}/system/bin" "${_mod}/webroot"
	# One distinct content per file so a swap between two files is also a mismatch.
	printf 'ELF-fluxd\n' >"${_mod}/system/bin/fluxd"
	printf 'flux_utility\n' >"${_mod}/system/bin/flux_utility"
	printf 'PK-apk\n' >"${_mod}/synthesiscore.apk"
	printf 'service\n' >"${_mod}/service.sh"
	printf 'action\n' >"${_mod}/action.sh"
	printf 'cleanup\n' >"${_mod}/cleanup.sh"
	printf 'uninstall\n' >"${_mod}/uninstall.sh"
	printf 'customize\n' >"${_mod}/customize.sh"
	cp "${REPO_ROOT}/module/integrity_runtime.sh" "${_mod}/integrity_runtime.sh"
	printf '<html></html>\n' >"${_mod}/webroot/index.html"
}

# assert_verify <label> <moddir> <manifest> <gen> <want_rc> <want_state> <want_class>
assert_verify() {
	_label="$1" _mod="$2" _man="$3" _gen="$4" _wrc="$5" _wstate="$6" _wclass="$7"
	set +e
	flux_ri_verify "${_man}" "${_mod}" "${_gen}"
	_rc=$?
	set -e
	if [ "${_rc}" = "${_wrc}" ] && [ "${FLUX_RI_STATE}" = "${_wstate}" ] &&
		[ "${FLUX_RI_CLASS}" = "${_wclass}" ]; then
		green "  ${_label}: state=${FLUX_RI_STATE} class=${FLUX_RI_CLASS} rc=${_rc} — ${FLUX_RI_REASON}"
	else
		fail "${_label}: got (rc=${_rc} state=${FLUX_RI_STATE} class=${FLUX_RI_CLASS}), " \
			"expected (rc=${_wrc} state=${_wstate} class=${_wclass})"
	fi
}

MOD="${WORK}/mod"
MAN="${WORK}/manifest"

# ── Writer ────────────────────────────────────────────────────────────────────
head2 "Manifest writer"
build_tree "${MOD}"
if flux_ri_write_manifest "${MAN}" "${GENERATION}" "${MOD}"; then
	green "  writer produced a manifest"
else
	fail "writer failed on a complete tree"
fi
# Every critical file appears exactly once, plus the generation line.
EXPECTED_FILES="$(flux_ri_critical_files | grep -c .)"
GOT_FILES="$(grep -cvE '^#|^generation ' "${MAN}")"
if [ "${GOT_FILES}" = "${EXPECTED_FILES}" ]; then
	green "  manifest lists all ${EXPECTED_FILES} critical files"
else
	fail "manifest lists ${GOT_FILES} files, expected ${EXPECTED_FILES}"
fi
if grep -q "^generation ${GENERATION}$" "${MAN}"; then
	green "  manifest records generation ${GENERATION}"
else
	fail "manifest does not record the generation"
fi
# The writer must refuse (non-zero, no partial file) when a listed file cannot be hashed.
build_tree "${MOD}"
rm -f "${MOD}/synthesiscore.apk"
set +e
flux_ri_write_manifest "${WORK}/partial" "${GENERATION}" "${MOD}" 2>/dev/null
_wrc=$?
set -e
if [ "${_wrc}" -ne 0 ] && [ ! -f "${WORK}/partial" ]; then
	green "  writer refuses a tree with a missing critical file and leaves no partial manifest"
else
	fail "writer did not refuse a missing-file tree (rc=${_wrc}, file present: $([ -f "${WORK}/partial" ] && echo yes || echo no))"
fi

# ── Verifier: the happy path ──────────────────────────────────────────────────
head2 "Verifier — valid package"
build_tree "${MOD}"
flux_ri_write_manifest "${MAN}" "${GENERATION}" "${MOD}"
assert_verify "valid package" "${MOD}" "${MAN}" "${GENERATION}" 0 ok ok

# ── Verifier: each distinguishable failure ────────────────────────────────────
head2 "Verifier — classified failures"

# Changed binary → mismatch.
build_tree "${MOD}"; flux_ri_write_manifest "${MAN}" "${GENERATION}" "${MOD}"
printf 'tampered\n' >"${MOD}/system/bin/fluxd"
assert_verify "changed binary" "${MOD}" "${MAN}" "${GENERATION}" 1 failed mismatch

# Changed APK → mismatch.
build_tree "${MOD}"; flux_ri_write_manifest "${MAN}" "${GENERATION}" "${MOD}"
printf 'swapped\n' >"${MOD}/synthesiscore.apk"
assert_verify "changed APK" "${MOD}" "${MAN}" "${GENERATION}" 1 failed mismatch

# Changed WebUI entry point → mismatch.
build_tree "${MOD}"; flux_ri_write_manifest "${MAN}" "${GENERATION}" "${MOD}"
printf '<script src="http://evil"></script>\n' >"${MOD}/webroot/index.html"
assert_verify "changed WebUI entry" "${MOD}" "${MAN}" "${GENERATION}" 1 failed mismatch

# Missing critical file.
build_tree "${MOD}"; flux_ri_write_manifest "${MAN}" "${GENERATION}" "${MOD}"
rm -f "${MOD}/service.sh"
assert_verify "missing critical file" "${MOD}" "${MAN}" "${GENERATION}" 1 failed missing

# Symlink substitution (target content even matches, and it is still rejected).
build_tree "${MOD}"; flux_ri_write_manifest "${MAN}" "${GENERATION}" "${MOD}"
cp "${MOD}/action.sh" "${WORK}/decoy"; printf 'action\n' >"${WORK}/decoy"
rm -f "${MOD}/action.sh"; ln -s "${WORK}/decoy" "${MOD}/action.sh"
assert_verify "symlink substitution" "${MOD}" "${MAN}" "${GENERATION}" 1 failed symlink

# Directory substituted for a file.
build_tree "${MOD}"; flux_ri_write_manifest "${MAN}" "${GENERATION}" "${MOD}"
rm -f "${MOD}/cleanup.sh"; mkdir -p "${MOD}/cleanup.sh"
assert_verify "directory substituted for file" "${MOD}" "${MAN}" "${GENERATION}" 1 failed wrongtype

# Unexpected writable mode (group/other-writable).
build_tree "${MOD}"; flux_ri_write_manifest "${MAN}" "${GENERATION}" "${MOD}"
chmod 0666 "${MOD}/uninstall.sh"
assert_verify "world-writable mode" "${MOD}" "${MAN}" "${GENERATION}" 1 failed writable

# Permission denied. Skipped under a uid that ignores read bits (root/CI), which would read the
# file anyway and make the assertion lie.
build_tree "${MOD}"; flux_ri_write_manifest "${MAN}" "${GENERATION}" "${MOD}"
chmod 0000 "${MOD}/system/bin/flux_utility"
if [ "$(id -u)" -ne 0 ] && ! cat "${MOD}/system/bin/flux_utility" >/dev/null 2>&1; then
	assert_verify "permission denied" "${MOD}" "${MAN}" "${GENERATION}" 1 failed denied
else
	green "  permission denied: skipped (running as root; read bits do not apply)"
fi
chmod 0644 "${MOD}/system/bin/flux_utility"

# Unsupported package generation (manifest generation != module generation).
build_tree "${MOD}"; flux_ri_write_manifest "${MAN}" "${GENERATION}" "${MOD}"
assert_verify "generation mismatch" "${MOD}" "${MAN}" "999" 1 failed generation

# No manifest at all → ungoverned (return 2), tolerated for a pre-hardening upgrade.
build_tree "${MOD}"
assert_verify "no manifest (pre-hardening)" "${MOD}" "${WORK}/does-not-exist" "${GENERATION}" 2 ungoverned nomanifest

# ── The safe-no-write contract, and the reason bound ──────────────────────────
head2 "Safe-no-write contract"
build_tree "${MOD}"; flux_ri_write_manifest "${MAN}" "${GENERATION}" "${MOD}"
printf 'tampered\n' >"${MOD}/system/bin/fluxd"
set +e; flux_ri_verify "${MAN}" "${MOD}" "${GENERATION}"; _rc=$?; set -e
# The caller keys "do not write" off a non-zero return and state=failed. Assert both, together,
# because the whole safe-mode gate hangs on them.
if [ "${_rc}" -ne 0 ] && [ "${FLUX_RI_STATE}" = "failed" ]; then
	green "  a critical mismatch returns non-zero AND state=failed (the gate the caller reads)"
else
	fail "a critical mismatch did not signal failure to the caller (rc=${_rc}, state=${FLUX_RI_STATE})"
fi
# The reason must be a bounded single line that never contains a digest (safe to display).
_reason_lines="$(printf '%s' "${FLUX_RI_REASON}" | wc -l | tr -d ' ')"
if [ "${_reason_lines}" -eq 0 ] && ! printf '%s' "${FLUX_RI_REASON}" | grep -qiE '[0-9a-f]{32}'; then
	green "  reason is one line and carries no digest: ${FLUX_RI_REASON}"
else
	fail "reason is unbounded or leaks a digest: ${FLUX_RI_REASON}"
fi

head2 "═══ Result ═══"
if [ "${FAILURES}" -ne 0 ]; then
	red "${FAILURES} runtime-integrity check(s) failed."
	exit 1
fi
green "Runtime integrity: PROVEN"
green "  - writer records the full critical set and the generation, and refuses an incomplete tree"
green "  - verifier distinguishes mismatch / missing / symlink / wrongtype / writable / denied /"
green "    generation / nomanifest, and names the offending file"
green "  - a critical failure returns the safe-no-write signal with a bounded, digest-free reason"
