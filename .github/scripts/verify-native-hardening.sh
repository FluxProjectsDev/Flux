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
# Release-hardening proof for the shipped fluxd binary.
#
# verify-native-telemetry.sh proves the right code is LINKED IN. This proves the shipped
# binary is HARDENED: minimal exported surface, exploit-mitigation flags present, no
# developer paths, and — critically — the required entry points still there. Hardening
# that removes a needed entry point is a regression, not a win, so this checks both
# directions from the build's own artifacts rather than from the makefiles' intent.
#
# Run it after `ndk-build`, from the repository root:
#
#   ndk-build -j"$(nproc --all)"
#   .github/scripts/verify-native-hardening.sh
#
# ## What it proves, per ABI
#
#   1. the shipped binary (libs/<abi>/fluxd) is stripped — no .symtab
#   2. the export map took effect: the dynamic symbol table exports no project (`flux::`)
#      symbol, and does not export `main`
#   3. exploit mitigations are present: non-executable stack (GNU_STACK RW, not RWE),
#      RELRO (GNU_RELRO segment) and immediate binding (BIND_NOW / FLAGS_1 NOW)
#   4. the binary is position-independent (ET_DYN / PIE)
#   5. no developer or CI-workspace path survived into the binary
#   6. the required entry point (`main`) is still present in the unstripped link output
#
# It writes the shipped binary's exported-symbol inventory to
# native-hardening-<abi>.exports for the proof artifact, so "before/after" is auditable
# after the fact (before this hardening the dynamic table exported every defined global;
# after it, the inventory is empty of project symbols).
#
# Exit codes: 0 all checks pass; 1 a check failed or the build output is missing.

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
# The NDK's llvm-* understand every object this build produces. Fall back to a system
# llvm-* only if the NDK is not exported. Same policy as verify-native-telemetry.sh.
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

READELF="$(find_tool llvm-readelf)" || READELF="$(find_tool readelf)" || {
	red "ERROR: no readelf found. Export ANDROID_NDK_HOME or install llvm."
	exit 1
}
NM="$(find_tool llvm-nm)" || NM="$(find_tool nm)" || {
	red "ERROR: no nm found."
	exit 1
}
info "readelf: ${READELF}"
info "nm:      ${NM}"

# ── Source guard ─────────────────────────────────────────────────────────────
# The binary checks below are the real proof. This is a cheap guard so the export map
# cannot be silently unwired from the build without a signal: the flag and the file must
# both be present. It is not a substitute for the binary proof — it is the thing that
# fails loudly in review when someone deletes the line.
head2 "Source guard: export map is wired in"
# shellcheck disable=SC2016  # the literal $(LOCAL_PATH) is the make text we are grepping for
if grep -q -- '--version-script=$(LOCAL_PATH)/fluxd.map' jni/Android.mk; then
	green "  jni/Android.mk references the fluxd export map"
else
	fail "jni/Android.mk no longer passes --version-script=\$(LOCAL_PATH)/fluxd.map"
fi
if [ -f jni/fluxd.map ]; then
	green "  jni/fluxd.map present"
else
	fail "jni/fluxd.map is missing"
fi

# ── ABIs ─────────────────────────────────────────────────────────────────────
ABIS="${ABIS:-$(sed -n 's/^APP_ABI[[:space:]]*:=[[:space:]]*//p' jni/Application.mk)}"
if [ -z "${ABIS}" ]; then
	red "ERROR: could not determine APP_ABI from jni/Application.mk"
	exit 1
fi
info "ABIs under proof: ${ABIS}"

if [ ! -d "${LIBS_DIR}" ]; then
	red "ERROR: no build output at ${LIBS_DIR}/ — run ndk-build first."
	exit 1
fi

# Developer / CI paths that must never survive into a shipped binary. These are prefixes,
# matched against the binary's printable strings. /home/runner is the GitHub-hosted runner
# workspace; /home/<user> and /Users/<user> are developer trees.
DEV_PATH_PATTERNS='/home/|/Users/|/root/'

for ABI in ${ABIS}; do
	head2 "═══ ABI: ${ABI} ═══"

	SHIPPED="${LIBS_DIR}/${ABI}/fluxd"
	UNSTRIPPED="${OBJ_DIR}/${ABI}/fluxd"

	if [ ! -f "${SHIPPED}" ]; then
		fail "no shipped binary at ${SHIPPED}"
		continue
	fi

	# ── 1. Stripped ──────────────────────────────────────────────────────────
	head2 "1. Shipped binary is stripped (${ABI})"
	# Read the section list into a variable, then test it. Piping readelf into `grep -q`
	# lets grep close the pipe early, readelf dies of SIGPIPE, and pipefail then reports the
	# whole run failed — the same trap verify-native-telemetry.sh documents.
	SECTIONS="$("${READELF}" --section-headers "${SHIPPED}" 2>/dev/null || true)"
	if [[ "${SECTIONS}" == *.symtab* ]]; then
		fail "${SHIPPED} still has a .symtab — the release binary is not stripped"
	else
		green "  no .symtab: the shipped binary is stripped"
	fi

	# ── 2. Export map took effect ────────────────────────────────────────────
	head2 "2. Exported dynamic symbols (${ABI})"
	# .dynsym survives stripping. `nm -D --defined-only` lists the dynamic symbols the
	# binary *exports* (defined, not undefined imports). After the version script, that set
	# must contain no project symbol and must not export main.
	EXPORTS="$("${NM}" -D -C --defined-only "${SHIPPED}" 2>/dev/null || true)"
	INVENTORY="native-hardening-${ABI}.exports"
	printf '%s\n' "${EXPORTS}" >"${INVENTORY}"
	EXPORT_COUNT="$(printf '%s\n' "${EXPORTS}" | grep -c . || true)"
	info "${EXPORT_COUNT} defined dynamic symbol(s); inventory written to ${INVENTORY}"

	if printf '%s\n' "${EXPORTS}" | grep -q 'flux::'; then
		fail "the shipped binary exports project symbols (flux::…) — the export map did not take effect"
		printf '%s\n' "${EXPORTS}" | grep 'flux::' | head -5 >&2
	else
		green "  no flux:: symbol is exported"
	fi
	# `main` is the C entry the executable is *entered at*, never something to export
	# dynamically. Its presence in the dynamic export set would mean the map missed.
	if printf '%s\n' "${EXPORTS}" | grep -qwE 'T main| main$'; then
		fail "the shipped binary dynamically exports main"
	else
		green "  main is not dynamically exported"
	fi

	# ── 3. Exploit mitigations ───────────────────────────────────────────────
	head2 "3. Exploit mitigations (${ABI})"
	SEGMENTS="$("${READELF}" -l "${SHIPPED}" 2>/dev/null || true)"
	DYNAMIC="$("${READELF}" -d "${SHIPPED}" 2>/dev/null || true)"

	# Non-executable stack: a GNU_STACK program header with flags RW (not RWE). If the
	# header is absent the loader assumes an executable stack, which is the unsafe default.
	STACK_LINE="$(printf '%s\n' "${SEGMENTS}" | grep 'GNU_STACK' || true)"
	if [ -z "${STACK_LINE}" ]; then
		fail "no GNU_STACK segment — the stack is executable by default"
	elif printf '%s\n' "${STACK_LINE}" | grep -qE 'RWE|R E'; then
		fail "GNU_STACK is executable: ${STACK_LINE}"
	else
		green "  non-executable stack (GNU_STACK)"
	fi

	# RELRO: a GNU_RELRO segment maps the relocated data read-only after load.
	if printf '%s\n' "${SEGMENTS}" | grep -q 'GNU_RELRO'; then
		green "  RELRO present (GNU_RELRO)"
	else
		fail "no GNU_RELRO segment — RELRO is not enabled"
	fi

	# Immediate binding: the whole GOT is resolved at load, so it can be made read-only.
	# Shown as (FLAGS) BIND_NOW or (FLAGS_1) NOW in the dynamic section.
	if printf '%s\n' "${DYNAMIC}" | grep -qE 'BIND_NOW|FLAGS_1.*NOW'; then
		green "  immediate binding (BIND_NOW / FLAGS_1 NOW)"
	else
		fail "no BIND_NOW: the binary uses lazy binding, so full RELRO is not in force"
	fi

	# ── 4. Position independent ──────────────────────────────────────────────
	head2 "4. Position independence (${ABI})"
	ETYPE="$("${READELF}" -h "${SHIPPED}" 2>/dev/null | sed -n 's/^[[:space:]]*Type:[[:space:]]*//p')"
	case "${ETYPE}" in
	*DYN*) green "  ET_DYN (PIE): ${ETYPE}" ;;
	*) fail "the shipped binary is not a PIE (Type: ${ETYPE:-unknown})" ;;
	esac

	# ── 5. No developer / workspace paths ────────────────────────────────────
	head2 "5. No developer paths in the binary (${ABI})"
	# strings over the whole file, including .rodata where a leaked __FILE__ would land.
	if strings -a "${SHIPPED}" 2>/dev/null | grep -qE "${DEV_PATH_PATTERNS}"; then
		fail "the shipped binary contains a developer/workspace path"
		# Print only the offending *prefix class*, never the full path, so the CI log does
		# not itself republish the path the check exists to remove.
		strings -a "${SHIPPED}" 2>/dev/null | grep -oE "${DEV_PATH_PATTERNS}" | sort -u | head -3 >&2
	else
		green "  no /home, /Users or /root path survived into the binary"
	fi

	# ── 6. Required entry point still present ─────────────────────────────────
	head2 "6. Required entry point (${ABI})"
	# Hardening must not delete what the process needs to start. The unstripped link output
	# keeps its .symtab; `main` must be a defined text symbol there. (The shipped binary is
	# stripped, so this is proven on the same link output the telemetry proof uses.)
	if [ ! -f "${UNSTRIPPED}" ]; then
		fail "no unstripped link output at ${UNSTRIPPED} to confirm entry points"
	else
		SYMS="$("${NM}" -C "${UNSTRIPPED}" 2>/dev/null || true)"
		if printf '%s\n' "${SYMS}" | grep -qE '^[0-9a-f]+ [Tt] main$'; then
			green "  main is defined in the link output"
		else
			fail "main is not a defined text symbol in ${UNSTRIPPED} — an entry point was hidden"
		fi
	fi
done

head2 "═══ Result ═══"
if [ "${FAILURES}" -ne 0 ]; then
	red "${FAILURES} hardening check(s) failed."
	red "Flux native release hardening: NOT PROVEN"
	exit 1
fi
green "Flux native release hardening: PROVEN"
green "  - shipped binary stripped; exports no project symbol and no main"
green "  - non-executable stack, RELRO and immediate binding present"
green "  - position-independent executable"
green "  - no developer or CI-workspace path in the binary"
green "  - the required entry point (main) remains in the link output"
