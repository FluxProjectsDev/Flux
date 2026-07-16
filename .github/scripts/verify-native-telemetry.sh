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
# Link and symbol proof for the Flux V2 telemetry runtime.
#
# Proves, from the real native build output rather than from the source tree, that:
#
#   1. every Flux V2 telemetry component is actually linked into the production fluxd
#   2. no legacy telemetry object, symbol or compilation unit survives anywhere in the build
#   3. the stripped binary that ships is the same link output the symbols were proven on
#
# Run it after `ndk-build`, from the repository root:
#
#   ndk-build -j"$(nproc --all)"
#   .github/scripts/verify-native-telemetry.sh
#
# ## Why this exists
#
# Grep over the source tree cannot prove what is in a binary. A stale object, a resurrected
# translation unit or a static library that still carries a removed implementation would all
# survive a clean grep. This script reads the build's own artifacts instead.
#
# ## Evidence layers
#
# Each layer is independent, and each must pass on its own:
#
#   A. object manifest  — the .o files ndk-build actually produced per module
#   B. archive members  — the members of each static library handed to the linker
#   C. binary symbols   — the symbol table of the unstripped linked executable
#   D. link identity    — the shipped (stripped) binary is the same link output as (C)
#
# Layers A and B catch a removed unit that is still compiled but gc-sectioned out of the final
# image; layer C catches anything that reaches the binary; layer D closes the gap between what
# was proven and what is packaged.
#
# ## On stripping
#
# `libs/<abi>/fluxd` — the binary that ships — is stripped and has no .symtab, so symbol proof
# on it is impossible by construction. ndk-build keeps the unstripped link output at
# `obj/local/<abi>/fluxd`. That is the same linker invocation's product, so layer C proves the
# symbols there and layer D proves the shipped file is that same output via its GNU build-id.
#
# Exit codes: 0 all layers proven; 1 a proof failed or the build output is missing.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

OBJ_DIR="${NDK_OUT:-obj}/local"
LIBS_DIR="${NDK_LIBS_OUT:-libs}"

red() { printf '\033[31m%s\033[0m\n' "$1"; }
green() { printf '\033[32m%s\033[0m\n' "$1"; }
info() { printf '\033[36m•\033[0m %s\n' "$1"; }
head2() { printf '\n\033[1m%s\033[0m\n' "$1"; }

FAILURES=0
fail() {
	red "FAIL: $1"
	FAILURES=$((FAILURES + 1))
}

# ── Toolchain ────────────────────────────────────────────────────────────────
# The NDK's llvm-nm/llvm-readelf understand every object this build can produce. Fall back to
# a system llvm-* only if the NDK is not exported; never fall back to binutils `nm`, which
# cannot read LLVM bitcode and would silently report an empty symbol table for an LTO object.
find_tool() {
	local name="$1" candidate
	if [ -n "${ANDROID_NDK_HOME:-}" ]; then
		for candidate in "${ANDROID_NDK_HOME}"/toolchains/llvm/prebuilt/*/bin/"${name}"; do
			if [ -x "${candidate}" ]; then
				printf '%s' "${candidate}"
				return 0
			fi
		done
	fi
	if command -v "${name}" >/dev/null 2>&1; then
		command -v "${name}"
		return 0
	fi
	return 1
}

NM="$(find_tool llvm-nm)" || {
	red "ERROR: llvm-nm not found. Export ANDROID_NDK_HOME or install llvm."
	exit 1
}
READELF="$(find_tool llvm-readelf)" || READELF="$(find_tool readelf)" || {
	red "ERROR: no readelf found."
	exit 1
}
info "nm:      ${NM}"
info "readelf: ${READELF}"

# ── What must be present ─────────────────────────────────────────────────────
# Components with an out-of-line definition in a static library. Those libraries are compiled
# without -flto, so their members are real ELF objects: the linker cannot inline across them
# into fluxd's bitcode, and a live component therefore MUST appear in the symbol table. An
# absence here is a real absence, not an optimisation artifact.
REQUIRED_LINKED_SYMBOLS=(
	"flux::telemetry::AtomicStatusWatcher"
	"flux::telemetry::TelemetryRuntime"
	"flux::telemetry::TelemetryIngestor"
	"flux::telemetry::TelemetryDecoder"
	"flux::telemetry::TelemetryFreshness"
	"flux::telemetry::RuntimeSnapshotAssembler"
	"FluxDecisionService"
)

# TelemetryStore is deliberately header-only (jni/engine/telemetry/TelemetryStore.hpp): every
# member is defined inline and there is no TelemetryStore.cpp. At -O2 its members are inlined
# into their callers and any residual linkonce_odr copy is discarded, so requiring a
# TelemetryStore symbol in the binary would assert an optimisation decision, not a fact about
# the build — it would flake the moment inlining changed.
#
# It is proven structurally instead: its header must exist, TelemetryRuntime must own it by
# value, and TelemetryRuntime must itself be proven linked by the symbol layer above. A
# TelemetryStore that was not linked in would take TelemetryRuntime with it.
HEADER_ONLY_HEADER="jni/engine/telemetry/TelemetryStore.hpp"
HEADER_ONLY_OWNER="jni/engine/telemetry/TelemetryRuntime.hpp"

# Objects that must exist in the build. If a component's translation unit stopped being
# compiled, the symbol layer might still pass via some other definition; this pins the units.
REQUIRED_OBJECTS=(
	"AtomicStatusWatcher.o"
	"TelemetryRuntime.o"
	"TelemetryIngestor.o"
	"TelemetryDecoder.o"
	"TelemetryFreshness.o"
	"RuntimeSnapshotAssembler.o"
	"DecisionAdapter.o"
	"DecisionEngine.o"
)

# ── What must be absent ──────────────────────────────────────────────────────
# The legacy telemetry implementation removed in Increment 2. Any of these reappearing in the
# build means the removal regressed. Patterns are matched against demangled symbol names.
#
# NOTE: these are deliberately narrow. `RuntimeSnapshotAssembler::foreground_from_synthesiscore`
# is a live V2 symbol that legitimately names the provider, so a blanket "synthesiscore" match
# would be a false positive. Match the removed identifiers, not the vendor word.
FORBIDDEN_SYMBOLS=(
	"SynthesisCoreReader"
	"synthesis_core_cache"
	"SynthesisCoreCache"
	"ProfilePolicy::evaluate"
	"PolicyInputs"
	"ThermalThresholds"
	"build_runtime_snapshot"
	"build_capabilities"
	"to_record_health"
)

# Translation units deleted in Increment 2. Their objects must not be produced by any module.
FORBIDDEN_OBJECTS=(
	"SynthesisCore.o"
	"ProfilePolicy.o"
)

# Static libraries deleted in Increment 2. ndk-build would emit these if a module returned.
FORBIDDEN_ARCHIVES=(
	"libSynthesisCore.a"
	"libProfilePolicy.a"
)

# ── ABIs ─────────────────────────────────────────────────────────────────────
ABIS="${ABIS:-$(sed -n 's/^APP_ABI[[:space:]]*:=[[:space:]]*//p' jni/Application.mk)}"
if [ -z "${ABIS}" ]; then
	red "ERROR: could not determine APP_ABI from jni/Application.mk"
	exit 1
fi
info "ABIs under proof: ${ABIS}"

if [ ! -d "${OBJ_DIR}" ]; then
	red "ERROR: no build output at ${OBJ_DIR}/ — run ndk-build first."
	exit 1
fi

# ── Structural proof for the header-only component ───────────────────────────
head2 "Layer 0: header-only component (TelemetryStore)"
if [ ! -f "${HEADER_ONLY_HEADER}" ]; then
	fail "${HEADER_ONLY_HEADER} is missing"
elif grep -q "TelemetryStore store_;" "${HEADER_ONLY_OWNER}"; then
	green "  PROVEN (structural): TelemetryStore is owned by value by TelemetryRuntime, which is"
	green "                       proven linked below. Header-only by design; no symbol expected."
else
	fail "TelemetryRuntime no longer owns a TelemetryStore by value — the structural proof for"
	fail "  the header-only TelemetryStore no longer holds. Re-establish it or give TelemetryStore"
	fail "  an out-of-line definition and move it to REQUIRED_LINKED_SYMBOLS."
fi

for ABI in ${ABIS}; do
	head2 "═══ ABI: ${ABI} ═══"

	EXE="${OBJ_DIR}/${ABI}/fluxd"
	SHIPPED="${LIBS_DIR}/${ABI}/fluxd"

	if [ ! -f "${EXE}" ]; then
		fail "no unstripped executable at ${EXE}"
		continue
	fi

	# ── Layer A: object manifest ─────────────────────────────────────────────
	head2 "Layer A: object manifest (${ABI})"
	OBJ_LIST="$(find "${OBJ_DIR}/${ABI}" -name '*.o' -printf '%f\n' 2>/dev/null | sort -u)"
	OBJ_COUNT="$(printf '%s\n' "${OBJ_LIST}" | grep -c . || true)"
	info "${OBJ_COUNT} objects produced"

	for obj in "${REQUIRED_OBJECTS[@]}"; do
		if grep -qxF "${obj}" <<<"${OBJ_LIST}"; then
			green "  present: ${obj}"
		else
			fail "required object not produced: ${obj}"
		fi
	done

	for obj in "${FORBIDDEN_OBJECTS[@]}"; do
		if grep -qxF "${obj}" <<<"${OBJ_LIST}"; then
			fail "removed translation unit is being compiled again: ${obj}"
			find "${OBJ_DIR}/${ABI}" -name "${obj}" >&2
		else
			green "  absent:  ${obj}"
		fi
	done

	for archive in "${FORBIDDEN_ARCHIVES[@]}"; do
		if [ -f "${OBJ_DIR}/${ABI}/${archive}" ]; then
			fail "removed static library was rebuilt: ${OBJ_DIR}/${ABI}/${archive}"
		else
			green "  absent:  ${archive}"
		fi
	done

	# ── Layer B: archive members ─────────────────────────────────────────────
	head2 "Layer B: static archive symbols (${ABI})"
	ARCHIVES="$(find "${OBJ_DIR}/${ABI}" -maxdepth 1 -name '*.a' | sort)"
	if [ -z "${ARCHIVES}" ]; then
		fail "no static archives found under ${OBJ_DIR}/${ABI}"
	fi
	ARCHIVE_SYMS="$(mktemp)"
	# shellcheck disable=SC2086
	"${NM}" --demangle --defined-only ${ARCHIVES} 2>/dev/null >"${ARCHIVE_SYMS}" || true
	info "$(wc -l <"${ARCHIVE_SYMS}") defined symbols across $(printf '%s\n' "${ARCHIVES}" | wc -l) archives"

	for pattern in "${FORBIDDEN_SYMBOLS[@]}"; do
		if grep -qF -- "${pattern}" "${ARCHIVE_SYMS}"; then
			fail "removed symbol is back in a static archive: ${pattern}"
			grep -F -- "${pattern}" "${ARCHIVE_SYMS}" | head -5 >&2
		fi
	done
	green "  no removed symbol is defined in any archive"

	# ── Layer C: linked binary symbols ───────────────────────────────────────
	head2 "Layer C: linked binary symbol table (${ABI})"
	BIN_SYMS="$(mktemp)"
	if ! "${NM}" --demangle "${EXE}" >"${BIN_SYMS}" 2>/dev/null; then
		fail "could not read a symbol table from ${EXE}"
		continue
	fi
	# An absence proof is only worth anything if the symbol table is really there: on a stripped
	# binary every "absent" below would pass vacuously. Assert the section exists rather than
	# guessing from a symbol count, which would be an arbitrary threshold that says nothing
	# about whether the table was stripped.
	# Read the section list into a variable first. Piping readelf's output straight into
	# `grep -q` looks equivalent but is not: grep exits on the first match, readelf then dies
	# of SIGPIPE, and `set -o pipefail` reports the whole pipeline as failed — so a binary that
	# *does* have a .symtab is reported as stripped. It only shows up on real output sizes,
	# where readelf is still writing when grep leaves.
	SECTIONS="$("${READELF}" --section-headers "${EXE}" 2>/dev/null || true)"
	if [[ "${SECTIONS}" != *.symtab* ]]; then
		fail "${EXE} has no .symtab — it is stripped, so layer C would prove nothing."
		fail "  ndk-build must keep obj/local/${ABI}/fluxd unstripped."
		continue
	fi
	SYM_COUNT="$(wc -l <"${BIN_SYMS}")"
	info "${SYM_COUNT} symbols in ${EXE} (.symtab present)"

	for pattern in "${REQUIRED_LINKED_SYMBOLS[@]}"; do
		if grep -qF -- "${pattern}" "${BIN_SYMS}"; then
			green "  linked:  ${pattern} ($(grep -cF -- "${pattern}" "${BIN_SYMS}") symbols)"
		else
			fail "V2 component is NOT linked into fluxd: ${pattern}"
		fi
	done

	for pattern in "${FORBIDDEN_SYMBOLS[@]}"; do
		if grep -qF -- "${pattern}" "${BIN_SYMS}"; then
			fail "removed symbol is linked into fluxd: ${pattern}"
			grep -F -- "${pattern}" "${BIN_SYMS}" | head -5 >&2
		else
			green "  absent:  ${pattern}"
		fi
	done

	# ── Layer D: shipped binary is this link output ──────────────────────────
	head2 "Layer D: link identity of the shipped binary (${ABI})"
	if [ ! -f "${SHIPPED}" ]; then
		fail "no shipped binary at ${SHIPPED}"
	else
		BUILD_ID_UNSTRIPPED="$("${READELF}" --notes "${EXE}" 2>/dev/null | sed -n 's/.*Build ID: \([0-9a-f]*\).*/\1/p' | head -1)"
		BUILD_ID_SHIPPED="$("${READELF}" --notes "${SHIPPED}" 2>/dev/null | sed -n 's/.*Build ID: \([0-9a-f]*\).*/\1/p' | head -1)"
		if [ -n "${BUILD_ID_UNSTRIPPED}" ] && [ -n "${BUILD_ID_SHIPPED}" ]; then
			if [ "${BUILD_ID_UNSTRIPPED}" = "${BUILD_ID_SHIPPED}" ]; then
				green "  PROVEN: build-id ${BUILD_ID_SHIPPED} — the shipped binary is the link output"
				green "          proven in layer C."
			else
				fail "the shipped binary is NOT the link output proven above"
				fail "  unstripped: ${BUILD_ID_UNSTRIPPED}"
				fail "  shipped:    ${BUILD_ID_SHIPPED}"
			fi
		else
			# Documented fallback: without a build-id the strongest remaining evidence that the
			# two files are one link output is that their code sections are byte-identical —
			# stripping removes symbols and debug data, never .text.
			info "no GNU build-id present; falling back to .text section comparison"
			TEXT_A="$("${READELF}" --hex-dump=.text "${EXE}" 2>/dev/null | sha256sum | cut -d' ' -f1)"
			TEXT_B="$("${READELF}" --hex-dump=.text "${SHIPPED}" 2>/dev/null | sha256sum | cut -d' ' -f1)"
			if [ -n "${TEXT_A}" ] && [ "${TEXT_A}" = "${TEXT_B}" ]; then
				green "  PROVEN (fallback): .text is byte-identical (sha256 ${TEXT_A})"
			else
				fail "shipped binary's .text differs from the proven link output"
			fi
		fi

		# The shipped file must be an ELF executable for the ABI it is filed under. A wrong-ABI
		# binary in the tree would install and then fail to exec on device.
		case "${ABI}" in
		arm64-v8a) WANT_MACHINE="AArch64" ;;
		armeabi-v7a) WANT_MACHINE="ARM" ;;
		x86_64) WANT_MACHINE="X86-64" ;;
		x86) WANT_MACHINE="Intel 80386" ;;
		*) WANT_MACHINE="" ;;
		esac
		if [ -n "${WANT_MACHINE}" ]; then
			GOT_MACHINE="$("${READELF}" --file-header "${SHIPPED}" 2>/dev/null | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')"
			if grep -qF "${WANT_MACHINE}" <<<"${GOT_MACHINE}"; then
				green "  machine: ${GOT_MACHINE} (correct for ${ABI})"
			else
				fail "${SHIPPED} is '${GOT_MACHINE}', expected ${WANT_MACHINE} for ${ABI}"
			fi
		fi
	fi

	rm -f "${BIN_SYMS}" "${ARCHIVE_SYMS}"
done

head2 "═══ Result ═══"
if [ "${FAILURES}" -ne 0 ]; then
	red "${FAILURES} proof(s) failed."
	red "Flux V2 telemetry link/symbol proof: NOT PROVEN"
	exit 1
fi
green "Flux V2 telemetry link/symbol proof: PROVEN"
green "  - every V2 telemetry component is linked into fluxd on every ABI"
green "  - no removed legacy telemetry object, archive or symbol exists in the build"
green "  - the shipped stripped binary is the same link output that was proven"
