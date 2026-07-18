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
# Flashable package validation.
#
# Usage: .github/scripts/verify-package.sh <flux-*.zip>
#
# Proves that what actually ships is consistent with what the build claims to be: the expected
# ABIs, a V2-telemetry-only daemon, the pinned SynthesisCore artifact, required attribution —
# and nothing else. Everything is read out of the produced ZIP, not out of the source tree.
#
# The point is asymmetric: a missing file is an inconvenience, but an *extra* file in a root
# module is a liability. A leaked key, a build artifact, a stale implementation or an
# uncommitted local edit riding along in the package are all things a source-tree check cannot
# see and a user cannot easily inspect after flashing.
#
# Exit codes: 0 package is clean; 1 a check failed.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

ZIP="${1:-}"
if [ -z "${ZIP}" ]; then
	printf '\033[31mUsage: %s <flux-*.zip>\033[0m\n' "$0" >&2
	exit 1
fi
if [ ! -f "${ZIP}" ]; then
	printf '\033[31mERROR: no such package: %s\033[0m\n' "${ZIP}" >&2
	exit 1
fi

red() { printf '\033[31m%s\033[0m\n' "$1"; }
green() { printf '\033[32m%s\033[0m\n' "$1"; }
info() { printf '\033[36m•\033[0m %s\n' "$1"; }
head2() { printf '\n\033[1m%s\033[0m\n' "$1"; }

FAILURES=0
fail() {
	red "FAIL: $1"
	FAILURES=$((FAILURES + 1))
}

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

head2 "Package: ${ZIP} ($(du -h "${ZIP}" | cut -f1))"
if ! unzip -tqq "${ZIP}" >/dev/null 2>&1; then
	fail "the package is not a readable zip"
	exit 1
fi
unzip -qo "${ZIP}" -d "${WORK}/pkg"

MANIFEST="${WORK}/manifest.txt"
(cd "${WORK}/pkg" && find . -type f | sed 's|^\./||' | sort) >"${MANIFEST}"
info "$(wc -l <"${MANIFEST}") files in the package"

# ── 1. ABIs ──────────────────────────────────────────────────────────────────
head2 "1. Expected ABIs"
ABIS="${ABIS:-$(sed -n 's/^APP_ABI[[:space:]]*:=[[:space:]]*//p' jni/Application.mk)}"
info "expected: ${ABIS}"

READELF="$(command -v llvm-readelf || command -v readelf)" || {
	red "ERROR: no readelf available"
	exit 1
}

for abi in ${ABIS}; do
	BIN="${WORK}/pkg/libs/${abi}/fluxd"
	if [ ! -f "${BIN}" ]; then
		fail "no fluxd for ABI ${abi} in the package"
		continue
	fi
	case "${abi}" in
	arm64-v8a) want="AArch64" ;;
	armeabi-v7a) want="ARM" ;;
	x86_64) want="X86-64" ;;
	x86) want="Intel 80386" ;;
	*) want="" ;;
	esac
	got="$("${READELF}" --file-header "${BIN}" 2>/dev/null | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')"
	if [ -n "${want}" ] && ! grep -qF "${want}" <<<"${got}"; then
		fail "libs/${abi}/fluxd is '${got}', expected ${want}"
	else
		green "  ${abi}: fluxd present, ${got}, $(du -h "${BIN}" | cut -f1)"
	fi
done

# A binary filed under an ABI Flux does not build for would install and then fail to exec.
for dir in "${WORK}"/pkg/libs/*/; do
	[ -d "${dir}" ] || continue
	abi="$(basename "${dir}")"
	if ! grep -qF " ${abi} " <<<" ${ABIS} "; then
		fail "package carries an unexpected ABI directory: libs/${abi}"
	fi
done

# ── 2. Nothing from the build tree ───────────────────────────────────────────
head2 "2. No build or source artifacts"
# A root module ships to a device; sources, objects and archives are pure weight and leak the
# build layout. `.map` and placeholders are already excluded by compile_zip.sh — assert it.
FORBIDDEN_GLOBS=(
	'*.o' '*.a' '*.cpp' '*.hpp' '*.cc' '*.h'
	'*.map' '*placeholder*' '.shellcheckrc'
	'*.keystore' '*.jks' '*.pem' '*.p12' '*.key' '*.pk8' '*.der'
	'*.apk.idsig'
)
while IFS= read -r entry; do
	base="${entry##*/}"
	for glob in "${FORBIDDEN_GLOBS[@]}"; do
		# shellcheck disable=SC2053  # glob is intentionally a pattern, not a literal
		if [[ "${base}" == ${glob} ]]; then
			fail "package contains a forbidden artifact (${glob}): ${entry}"
		fi
	done
done <"${MANIFEST}"
green "  no objects, archives, sources, linker maps, placeholders or key material"

# Whole directories that must never be packaged.
for dir in .git .github jni webui external obj docs dependencies; do
	if [ -e "${WORK}/pkg/${dir}" ]; then
		fail "package contains the '${dir}' directory"
	fi
done
green "  no .git, .github, jni, webui, external, obj, docs or dependencies directory"

# ── 3. The protected local WIP is not riding along ───────────────────────────
head2 "3. Protected working-tree files"
# CI builds from a clean checkout so this cannot trigger there; it exists to stop a *local*
# packaging run from shipping uncommitted edits, which is exactly how an unreviewed change
# reaches a device unnoticed.
for wip in build.yml flux_utility.sh; do
	if grep -qxF "${wip}" "${MANIFEST}" || grep -q "/${wip}$" "${MANIFEST}"; then
		fail "package contains '${wip}' at an unexpected path"
	fi
done
# flux_utility legitimately ships as system/bin/flux_utility (extension stripped). If it ships,
# it must be the committed version, not a local edit.
SHIPPED_UTIL="${WORK}/pkg/system/bin/flux_utility"
if [ -f "${SHIPPED_UTIL}" ]; then
	if git rev-parse --git-dir >/dev/null 2>&1; then
		if git show HEAD:scripts/flux_utility.sh 2>/dev/null | diff -q - "${SHIPPED_UTIL}" >/dev/null 2>&1; then
			green "  system/bin/flux_utility matches the committed scripts/flux_utility.sh"
		else
			fail "system/bin/flux_utility differs from the committed scripts/flux_utility.sh —"
			fail "  the package was built from uncommitted local changes. Do not ship it."
		fi
	fi
else
	fail "system/bin/flux_utility is missing from the package"
fi

# ── 4. No removed legacy telemetry implementation ────────────────────────────
head2 "4. Legacy telemetry absent"
LEGACY_NAMES=(SynthesisCore.cpp SynthesisCore.hpp ProfilePolicy.cpp ProfilePolicy.hpp synthesis_core.json)
for name in "${LEGACY_NAMES[@]}"; do
	if grep -q "\(^\|/\)${name}$" "${MANIFEST}"; then
		fail "package ships a removed legacy telemetry artifact: ${name}"
	else
		green "  absent: ${name}"
	fi
done

# Dev/CI-only tooling must not be packaged: it is dead weight in a user-facing artifact and
# implies a workflow that does not exist on a device.
for name in fetch-synthesiscore verify-synthesiscore update-synthesiscore-lock; do
	if grep -q "\(^\|/\)${name}\(\.sh\)\?$" "${MANIFEST}"; then
		fail "package ships the dev-only script '${name}' — it is CI tooling, not a device"
		fail "  runtime script, and customize.sh never installs it."
	else
		green "  absent: ${name} (dev-only)"
	fi
done

# ── 4b. No legacy shell apply payload ────────────────────────────────────────
head2 "4b. Legacy shell applier absent"
# Read out of the produced ZIP, not out of compile_zip.sh: the packaging script's intent and the
# package's contents are different facts, and only one of them lands on a device. A stale
# module/system/bin left over from an earlier build would ship a working copy of the old write
# path even with the source clean.
for name in flux_profiler flux_profiler.sh Profiler.cpp Profiler.hpp; do
	if grep -q "\(^\|/\)${name}$" "${MANIFEST}"; then
		fail "package ships the removed legacy applier artifact: ${name}"
		fail "  the V2 ExecutionEngine applies profiles; shipping the shell applier would put a"
		fail "  runnable copy of the old write path on every device"
	else
		green "  absent: ${name}"
	fi
done

# ── 4c. The V2 runtime and required assets are still there ───────────────────
head2 "4c. Required artifacts present"
# The other half of a deletion increment: proving that what should have gone, went — and that
# nothing else went with it. A package that is missing the daemon passes every absence check in
# this file.
REQUIRED_ENTRIES=(
	"system/bin/fluxd"
	"system/bin/flux_utility"
	"synthesiscore.apk"
	"customize.sh"
	"uninstall.sh"
	"service.sh"
	"module.prop"
)
for entry in "${REQUIRED_ENTRIES[@]}"; do
	if grep -q "\(^\|/\)${entry##*/}$" "${MANIFEST}"; then
		green "  present: ${entry}"
	else
		fail "package is missing a required artifact: ${entry}"
	fi
done

if grep -qE "webroot/.+" "${MANIFEST}"; then
	green "  present: webroot/ (WebUI assets)"
else
	fail "package ships no WebUI assets"
fi

# ── 5. Pinned SynthesisCore artifact ─────────────────────────────────────────
head2 "5. Bundled SynthesisCore artifact"
PKG_APK="${WORK}/pkg/synthesiscore.apk"
if [ ! -f "${PKG_APK}" ]; then
	fail "no synthesiscore.apk in the package — the telemetry provider would never start"
else
	LOCK_SHA="$(grep -E '^sha256=' dependencies/synthesiscore.lock | cut -d= -f2-)"
	PKG_SHA="$(sha256sum "${PKG_APK}" | cut -d' ' -f1)"
	if [ "${PKG_SHA}" = "${LOCK_SHA}" ]; then
		green "  the packaged APK is byte-identical to the pinned release (${PKG_SHA})"
	else
		fail "the packaged APK is NOT the pinned artifact"
		fail "  lock:     ${LOCK_SHA}"
		fail "  packaged: ${PKG_SHA}"
	fi
	# compile_zip.sh generates a .sha256 next to every shipped file; if it ships, it must be true.
	if [ -f "${PKG_APK}.sha256" ]; then
		STATED="$(cat "${PKG_APK}.sha256")"
		if [ "${STATED}" = "${PKG_SHA}" ]; then
			green "  the package's own synthesiscore.apk.sha256 agrees"
		else
			fail "synthesiscore.apk.sha256 states ${STATED} but the file hashes to ${PKG_SHA}"
		fi
	fi
fi

# ── 6. module.prop ───────────────────────────────────────────────────────────
head2 "6. module.prop"
PROP="${WORK}/pkg/module.prop"
if [ ! -f "${PROP}" ]; then
	fail "no module.prop — the package is not a valid root module"
else
	prop() { sed -n "s/^$1=//p" "${PROP}" | head -1; }
	VERSION="$(prop version)"
	VERSION_CODE="$(prop versionCode)"
	UPDATE_JSON="$(prop updateJson)"
	NAME="$(prop name)"
	info "name=${NAME}"
	info "version=${VERSION}"
	info "versionCode=${VERSION_CODE}"
	info "updateJson=${UPDATE_JSON}"

	# The module identity a package manager displays is exactly "Flux". "Flux Tweaks" was the
	# pre-review name and must not reappear in the packaged metadata.
	if [ "${NAME}" = "Flux" ]; then
		green "  name is exactly 'Flux'"
	else
		fail "module.prop name must be 'Flux', got '${NAME}'"
	fi
	if grep -q "Flux Tweaks" "${PROP}"; then
		fail "packaged module.prop still carries the retired name 'Flux Tweaks'"
	fi

	if ! grep -qE '^[0-9]+$' <<<"${VERSION_CODE}"; then
		fail "versionCode '${VERSION_CODE}' is not an integer; module managers compare it numerically"
	else
		green "  versionCode is an integer"
	fi
	# A literal placeholder means the stamping step did not run.
	if grep -qiE 'placeholder|@version' <<<"${VERSION}"; then
		fail "version was never stamped: '${VERSION}'"
	else
		green "  version is stamped"
	fi
	# The update manifest must point at the repository that built this package. Advertising
	# someone else's releases is how a fork silently updates users to a foreign build.
	if [ -n "${GITHUB_REPOSITORY:-}" ]; then
		if grep -qF "github.com/${GITHUB_REPOSITORY}/" <<<"${UPDATE_JSON}"; then
			green "  updateJson points at this repository (${GITHUB_REPOSITORY})"
		else
			fail "updateJson '${UPDATE_JSON}' does not point at ${GITHUB_REPOSITORY}"
		fi
	fi
	# Category E: no Encore-owned infrastructure, ever.
	if grep -qiE 'rem01|encore' <<<"${UPDATE_JSON}"; then
		fail "updateJson points at Encore infrastructure: ${UPDATE_JSON}"
	fi
fi

# ── 7. Attribution ───────────────────────────────────────────────────────────
head2 "7. Required attribution ships"
# Apache-2.0 obliges the NOTICE to travel with the distribution. This check exists to stop a
# well-meaning "clean up the package" change from dropping it.
for required in LICENSE NOTICE.md; do
	if grep -qxF "${required}" "${MANIFEST}"; then
		green "  present: ${required}"
	else
		fail "required attribution file is missing from the package: ${required}"
	fi
done
if [ -f "${WORK}/pkg/NOTICE.md" ]; then
	if grep -qi "rem01gaming\|encore" "${WORK}/pkg/NOTICE.md"; then
		green "  NOTICE.md still carries the upstream Encore attribution (required, do not remove)"
	else
		fail "NOTICE.md no longer attributes Encore. Attribution is a licence obligation and"
		fail "  must not be removed by a provenance cleanup."
	fi
fi

# ── 8. No Encore operational asset ───────────────────────────────────────────
head2 "8. No Encore operational asset or metadata"
# Attribution in NOTICE.md/LICENSE and source headers is required and expected. What must not
# ship is Encore *operational* material: its binaries, its gamelist, its config, its endpoints.
ENCORE_HITS="$(grep -iE '(^|/)(encore|rem01)' "${MANIFEST}" || true)"
if [ -n "${ENCORE_HITS}" ]; then
	fail "package contains Encore-named files:"
	printf '%s\n' "${ENCORE_HITS}" >&2
else
	green "  no Encore-named file in the package"
fi
# Any *operational* network destination pointing at Encore infrastructure would make Flux
# depend on it at runtime. An Encore URL inside a comment is the opposite: it is the Apache-2.0
# attribution for adapted files, which is required to ship and must never be "cleaned up".
# So this looks only at code — comment lines are stripped before matching — which is where a
# real dependency (a curl/wget target, an update endpoint) would have to live.
#
# The match deliberately does not pipe sed into `grep -q`. Under `set -o pipefail` that
# combination is actively unsafe here: grep exits at the first match, sed dies of SIGPIPE, the
# pipeline reports failure, and the `if` concludes "no match" — the check would go quiet
# exactly when it found a real Encore endpoint. A verifier that passes when it should fail is
# worse than no verifier, so the stripped text is materialised first and matched without a pipe.
ENDPOINT_HITS=""
while IFS= read -r f; do
	# Skip binaries up front (grep -I reports no match on them). A URL cannot hide in fluxd or
	# the APK as *code* we could act on, and feeding them to sed only makes bash warn about
	# discarding null bytes for every binary in the package.
	grep -Iq . "${f}" 2>/dev/null || continue
	stripped="$(sed -E 's@(^|[[:space:]])(#|//).*$@@' "${f}" 2>/dev/null || true)"
	if grep -qIE 'https?://[^"[:space:]]*(rem01|encore)' <<<"${stripped}"; then
		ENDPOINT_HITS="${ENDPOINT_HITS}${f}"$'\n'
	fi
done < <(find "${WORK}/pkg" -type f ! -name 'NOTICE.md' ! -name 'LICENSE' ! -name '*.sha256')
if [ -n "${ENDPOINT_HITS}" ]; then
	fail "package references an Encore endpoint in executable code:"
	printf '%s' "${ENDPOINT_HITS}" >&2
else
	green "  no Encore endpoint in executable code (attribution comments are required and kept)"
fi

# ── 9. No release or signing output ──────────────────────────────────────────
head2 "9. No release or signing output"
# The flashable zip is not a release: release assets are produced by the dispatch-gated publish
# step and must never be baked into the module.
for name in update.json changelog.md; do
	if grep -qxF "${name}" "${MANIFEST}"; then
		fail "release asset '${name}' is inside the module package"
	else
		green "  absent: ${name} (release asset, not module content)"
	fi
done

head2 "═══ Result ═══"
if [ "${FAILURES}" -ne 0 ]; then
	red "${FAILURES} package check(s) failed."
	red "Flashable package validation: FAILED"
	exit 1
fi
green "Flashable package validation: PASSED"
