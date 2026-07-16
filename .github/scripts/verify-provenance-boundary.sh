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
# Enforce the Category A / Category C boundary in CI.
#
# The whole reason Flux can keep an independent execution engine while still shipping
# Encore-derived device knowledge is that the two live in different places and obey different
# rules:
#
#   jni/engine/execution/  Category A — Flux-authored mechanism. Writes to devices.
#   jni/device/            Category C — derived knowledge. Declarative data. Never writes.
#
# That boundary is a licensing position, not a style preference. If device tables ever start
# containing control flow, the engine's independence claim collapses along with it — so this
# check fails the build rather than a review.
#
# Host tests already assert this from the inside (the descriptors themselves carry no
# executable content). This asserts it from the outside, at the file level, where a new
# contributor is most likely to get it wrong by adding "just one helper".

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

DEVICE_DIR="jni/device"
ENGINE_DIR="jni/engine/execution"

red() { printf '\033[31m%s\033[0m\n' "$1"; }
green() { printf '\033[32m%s\033[0m\n' "$1"; }
info() { printf '\033[36m•\033[0m %s\n' "$1"; }
head2() { printf '\n\033[1m%s\033[0m\n' "$1"; }

FAILURES=0
fail() {
	red "FAIL: $1"
	FAILURES=$((FAILURES + 1))
}

if [ ! -d "${DEVICE_DIR}" ]; then
	red "ERROR: ${DEVICE_DIR} does not exist"
	exit 1
fi

DEVICE_SOURCES="$(find "${DEVICE_DIR}" -name '*.cpp' -o -name '*.hpp' | sort)"
if [ -z "${DEVICE_SOURCES}" ]; then
	red "ERROR: no sources found under ${DEVICE_DIR}"
	exit 1
fi
info "device pack sources: $(printf '%s\n' "${DEVICE_SOURCES}" | wc -l)"

# ── 1. Device packs must not write ───────────────────────────────────────────
head2 "1. Device packs perform no I/O"
# The engine is the only thing that writes. A device table reaching for any of these means the
# data has started doing the mechanism's job.
FORBIDDEN_CALLS=(
	"write_checked" "write(" "SysfsNodeBackend" "NodeBackend"
	"::open" "fopen" "ofstream" "chmod" "fchmod"
	"system(" "popen" "execv" "fork(" "std::filesystem"
)
for f in ${DEVICE_SOURCES}; do
	# Strip comments before matching: this file's own prose legitimately names these calls, and
	# so does the device README's explanation of why they are banned.
	stripped="$(sed -E 's://.*$::' "${f}" | sed -E '/^\s*\*/d' | sed -E 's:/\*.*\*/::')"
	for call in "${FORBIDDEN_CALLS[@]}"; do
		if grep -qF -- "${call}" <<<"${stripped}"; then
			fail "${f} references '${call}' — device packs are data and must not perform I/O"
		fi
	done
done
[ "${FAILURES}" -eq 0 ] && green "  no filesystem, process, or backend calls in ${DEVICE_DIR}"

# ── 2. Device packs must not decide policy ───────────────────────────────────
head2 "2. Device packs select no profiles"
# Which profile to run is the Decision Engine's job. A descriptor only says what a given intent
# means for one node; a table that starts reading telemetry is a second decision path.
POLICY_SYMBOLS=("DecisionEngine" "PolicyDecision" "TargetProfile" "RuntimeSnapshot" "TelemetryRuntime")
for f in ${DEVICE_SOURCES}; do
	stripped="$(sed -E 's://.*$::' "${f}" | sed -E '/^\s*\*/d')"
	for symbol in "${POLICY_SYMBOLS[@]}"; do
		if grep -qF -- "${symbol}" <<<"${stripped}"; then
			fail "${f} references '${symbol}' — device packs must not participate in profile selection"
		fi
	done
done
green "  no decision-path symbols in ${DEVICE_DIR}"

# ── 3. Derived data must carry attribution ───────────────────────────────────
head2 "3. Attribution travels with derived data"
# Apache-2.0 obliges it, and the provenance matrix is the authoritative record. Both must agree
# with the source, or one of them is lying to whoever reads it.
for f in ${DEVICE_SOURCES}; do
	case "${f}" in
	*/README.md) continue ;;
	esac
	if ! grep -q "Rem01Gaming" "${f}"; then
		fail "${f} carries no upstream attribution; derived device data must name its origin"
	fi
	if ! grep -q "Apache License" "${f}"; then
		fail "${f} carries no Apache-2.0 notice"
	fi
	if ! grep -qi "encore" "${f}"; then
		fail "${f} does not name Encore Tweaks as the origin of its device knowledge"
	fi
done
green "  every device pack source carries copyright, licence, and origin"

if ! grep -q "jni/device" docs/provenance-matrix.md; then
	fail "docs/provenance-matrix.md has no record for jni/device — retained derived data must be"
	fail "  recorded in the authoritative provenance record"
else
	green "  jni/device is recorded in the provenance matrix"
fi

if ! grep -q "jni/device" NOTICE.md; then
	fail "NOTICE.md does not mention jni/device"
else
	green "  jni/device is recorded in NOTICE.md"
fi

# ── 4. The engine must not depend on device tables ───────────────────────────
head2 "4. The engine does not depend on device knowledge"
# The dependency runs one way. The engine consumes descriptors as an abstract type; if it ever
# includes a specific device's table, the mechanism has learned about a device and Category A
# is gone.
for f in $(find "${ENGINE_DIR}" -name '*.cpp' -o -name '*.hpp' | sort); do
	if grep -qE '#include\s*[<"].*DevicePacks' "${f}"; then
		fail "${f} includes DevicePacks — the Flux-owned engine must not depend on derived device data"
	fi
	if grep -qE '\bflux::device\b' "${f}"; then
		fail "${f} references flux::device — the engine must stay device-agnostic"
	fi
done
green "  ${ENGINE_DIR} has no dependency on ${DEVICE_DIR}"

# ── 5. The engine must not call the legacy shell ─────────────────────────────
head2 "5. The engine does not shell out"
for f in $(find "${ENGINE_DIR}" -name '*.cpp' -o -name '*.hpp' | sort); do
	stripped="$(sed -E 's://.*$::' "${f}" | sed -E '/^\s*\*/d')"

	for call in "flux_profiler" "flux_utility"; do
		if grep -qF -- "${call}" <<<"${stripped}"; then
			fail "${f} references '${call}' — the V2 engine must not depend on the legacy shell helpers"
		fi
	done

	# Match a *call*, not a mention. DescriptorValidator::looks_executable legitimately holds
	# "system(" and friends as string literals — it is the thing that detects them — so a plain
	# substring search flags the detector for containing the pattern it detects. Requiring the
	# name not to be preceded by a quote distinguishes `system(x)` from `"system("`.
	for call in system popen execv fork; do
		if grep -qE "(^|[^\"a-zA-Z_.>:])${call}[[:space:]]*\(" <<<"${stripped}"; then
			fail "${f} calls '${call}(' — the V2 engine writes through SysfsNodeBackend, never a subprocess"
		fi
	done
done
green "  no legacy shell dependency or subprocess call in ${ENGINE_DIR}"

# ── 6. The planning path cannot write ────────────────────────────────────────
head2 "6. Intent mapping and dry-run planning are inert"
# The dry run's entire value is that it can be computed without touching the device. That is
# only true while the planner has no way to write, so this proves the property structurally
# rather than trusting the planner to keep choosing not to.
#
# The planner holds a `const CapabilityProbe &`, and the probe reads. Nothing in these four
# files may reach a mutating backend call, because a plan that can write is not a plan.
PLANNING_SOURCES="PolicyIntent.hpp PolicyIntent.cpp DryRunPlanner.hpp DryRunPlanner.cpp"
MUTATING_CALLS=("write_checked" "probe_writable" "fchmod" "chmod" "::write" "O_WRONLY" "O_RDWR" "O_CREAT")
for base in ${PLANNING_SOURCES}; do
	f="${ENGINE_DIR}/${base}"
	if [ ! -f "${f}" ]; then
		fail "${f} is missing — the planning path must exist to be proven inert"
		continue
	fi
	stripped="$(sed -E 's://.*$::' "${f}" | sed -E '/^\s*\*/d' | sed -E 's:/\*.*\*/::')"
	for call in "${MUTATING_CALLS[@]}"; do
		if grep -qF -- "${call}" <<<"${stripped}"; then
			fail "${f} references '${call}' — planning must never be able to mutate a device node"
		fi
	done
done
green "  no mutating backend call reachable from intent mapping or dry-run planning"

# ── 7. Intents name outcomes, not nodes ──────────────────────────────────────
head2 "7. PolicyIntent carries no device paths"
# The split that makes the vendor boundary hold: intents say *what* should be true, descriptors
# say *which node* expresses it. The moment an intent hard-codes a path, policy has learned a
# device again and the Category C isolation is decorative.
INTENT_FILES="PolicyIntent.hpp PolicyIntent.cpp"
for base in ${INTENT_FILES}; do
	f="${ENGINE_DIR}/${base}"
	stripped="$(sed -E 's://.*$::' "${f}" | sed -E '/^\s*\*/d' | sed -E 's:/\*.*\*/::')"
	# A device path inside a string literal. Comments are stripped first because the prose in
	# these files legitimately explains the rule using the paths it forbids.
	if grep -qE '"/(sys|proc|dev)/' <<<"${stripped}"; then
		fail "${f} contains a device path literal — intents name outcomes; only descriptors name nodes"
	fi
	for symbol in "CapabilityDescriptor" "DevicePack" "PathPolicy"; do
		if grep -qF -- "${symbol}" <<<"${stripped}"; then
			fail "${f} references '${symbol}' — the intent vocabulary must stay independent of any device table"
		fi
	done
done
green "  intents are expressed as outcomes, with no path literal and no descriptor dependency"

head2 "═══ Result ═══"
if [ "${FAILURES}" -ne 0 ]; then
	red "${FAILURES} provenance-boundary violation(s)."
	red "Category A / Category C boundary: VIOLATED"
	exit 1
fi
green "Category A / Category C boundary: INTACT"
green "  - device packs are data: no I/O, no policy, no backend access"
green "  - derived data carries copyright, licence, origin, NOTICE and matrix records"
green "  - the Flux-owned engine depends on no device knowledge and no legacy shell"
green "  - intent mapping and dry-run planning cannot write, chmod, or name a device path"
