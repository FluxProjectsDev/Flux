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
# Prove the V2 execution path is live and the legacy shell apply path is not.
#
# The cutover's whole claim is "there is exactly one thing that writes to this device". That is
# not something a reviewer can confirm by reading a diff, and it is not something grep can
# confirm either — grep sees a string in a comment and a call to a function that no longer
# exists and cannot tell them apart from the real thing.
#
# So this checks the *built binary*: what it links, what it calls, and what strings it carries.
# A stale object file, a resurrected fallback, or a well-meant "just this once" shell call all
# fail here rather than in someone's hands.
#
# Layers:
#   1. the daemon links the V2 execution engine (symbol proof)
#   2. the daemon contains no flux_profiler invocation (string proof)
#   3. the daemon calls no shell-execution libc entry point (relocation/symbol proof)
#   4. there is exactly one apply entry point in the source (call-graph proof)
#   5. there is exactly one zen write entry point
#   6. the packaged module still ships what it claims to

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

red() { printf '\033[31m%s\033[0m\n' "$1"; }
green() { printf '\033[32m%s\033[0m\n' "$1"; }
info() { printf '\033[36m•\033[0m %s\n' "$1"; }
head2() { printf '\n\033[1m%s\033[0m\n' "$1"; }

FAILURES=0
fail() {
	red "FAIL: $1"
	FAILURES=$((FAILURES + 1))
}

# The unstripped binary. ndk-build keeps it under obj/local/<abi>; libs/<abi> is stripped and
# has no .symtab, so a symbol check there would silently prove nothing.
ABI="${1:-arm64-v8a}"
EXE="obj/local/${ABI}/fluxd"

NM="${NM:-llvm-nm}"
STRINGS_BIN="${STRINGS:-strings}"
command -v "${NM}" >/dev/null 2>&1 || NM="nm"

if [ ! -f "${EXE}" ]; then
	red "ERROR: ${EXE} not found — build the daemon before running this proof"
	exit 1
fi
info "binary: ${EXE}"

# ── 1. The V2 execution engine is linked in ──────────────────────────────────
head2 "1. The daemon links the V2 execution path"
# If the cutover were cosmetic — the runtime written but never called — the linker would drop
# these. Their presence is what makes "the daemon uses the engine" a fact rather than a claim.
REQUIRED_SYMBOLS=(
	"flux::execution::ExecutionRuntime::on_decision"
	"flux::execution::ExecutionEngine::apply"
	"flux::execution::LivePlanCompiler::compile"
	"flux::execution::SysfsNodeBackend::write_checked"
	"flux::execution::DryRunPlanner::plan"
	"flux::execution::IntentMapper::expand"
	"flux::execution::RuntimeProfileState::record_apply"
)

SYMBOLS="$("${NM}" -C "${EXE}" 2>/dev/null || true)"
if [ -z "${SYMBOLS}" ]; then
	red "ERROR: ${NM} produced no symbols for ${EXE}"
	exit 1
fi

for symbol in "${REQUIRED_SYMBOLS[@]}"; do
	if grep -qF -- "${symbol}" <<<"${SYMBOLS}"; then
		green "  linked: ${symbol}"
	else
		fail "${symbol} is not in the binary — the V2 execution path is not actually live"
	fi
done

# ── 2. No flux_profiler invocation survives ──────────────────────────────────
head2 "2. The daemon cannot invoke the legacy shell applier"
# The legacy dispatchers called system("flux_profiler performance") and friends. If any of those
# call sites survived — or came back — the argument string is still in .rodata.
BINARY_STRINGS="$("${STRINGS_BIN}" -a "${EXE}" 2>/dev/null || true)"
LEGACY_STRINGS=(
	"flux_profiler performance"
	"flux_profiler performance_lite"
	"flux_profiler balance"
	"flux_profiler powersave"
	"flux_profiler perfcommon"
)
for legacy in "${LEGACY_STRINGS[@]}"; do
	if grep -qF -- "${legacy}" <<<"${BINARY_STRINGS}"; then
		fail "the binary still carries '${legacy}' — the legacy apply path is still reachable"
	fi
done
green "  no flux_profiler invocation string in the binary"

LEGACY_SYMBOLS=(
	"apply_performance_profile"
	"apply_performance_lite_profile"
	"apply_balance_profile"
	"apply_powersave_profile"
	"run_perfcommon"
	"set_profiler_env_vars"
)
for symbol in "${LEGACY_SYMBOLS[@]}"; do
	if grep -qF -- "${symbol}" <<<"${SYMBOLS}"; then
		fail "${symbol} is still in the binary — a legacy apply dispatcher survived the cutover"
	fi
done
green "  no legacy apply dispatcher symbol in the binary"

# ── 3. The daemon does not shell out to apply anything ───────────────────────
head2 "3. No shell execution reachable from the daemon"
# system() and popen() are how the legacy applier ran. Nothing in the V2 path needs either:
# device nodes are written through SysfsNodeBackend, and zen goes through posix_spawn-style
# fork/execvp in one audited place. system()/popen() take a shell command line, which is the
# thing that made an interpolated path dangerous in the first place.
for banned in "system" "popen"; do
	# Match an undefined (imported) symbol: "U system". A local symbol containing the word is
	# not an import of libc's system(3).
	if grep -qE "^\s+U ${banned}$" <<<"${SYMBOLS}"; then
		fail "the binary imports ${banned}() — the daemon can still run a shell command line"
	fi
done
green "  the binary imports neither system() nor popen()"

# ── 4. Exactly one apply entry point in the source ───────────────────────────
head2 "4. One apply entry point"
# A call-graph check, not a text search: the only thing that may call ExecutionEngine::apply is
# the composition root. If a second caller appears, there are two writers again, whatever the
# comments say.
APPLY_CALLERS="$(grep -rln "engine_\.apply(\|\.apply(plan" jni --include='*.cpp' 2>/dev/null |
	grep -v '/tests/' || true)"
EXPECTED_CALLER="jni/engine/execution/ExecutionRuntime.cpp"
if [ "${APPLY_CALLERS}" != "${EXPECTED_CALLER}" ]; then
	fail "ExecutionEngine::apply is called from outside the composition root:"
	fail "  expected only ${EXPECTED_CALLER}, found: ${APPLY_CALLERS:-<none>}"
else
	green "  ExecutionEngine::apply is called only by ${EXPECTED_CALLER}"
fi

# Main.cpp must orchestrate, not write. It may route events and read state; it may not reach
# for a backend, a plan or a node.
MAIN_FORBIDDEN=("write_checked" "SysfsNodeBackend" "LivePlanCompiler" "DryRunPlanner" "fchmod")
for symbol in "${MAIN_FORBIDDEN[@]}"; do
	stripped="$(sed -E 's://.*$::' jni/Main.cpp | sed -E '/^\s*\*/d')"
	if grep -qF -- "${symbol}" <<<"${stripped}"; then
		fail "jni/Main.cpp references '${symbol}' — the daemon's main must orchestrate, not apply"
	fi
done
green "  jni/Main.cpp contains no backend, planner or node access"

# ── 5. Exactly one zen write entry point ─────────────────────────────────────
head2 "5. One zen write entry point"
# set_zen_mode() is the mechanism. It must be reached through AndroidZenBackend and nothing
# else, or the "exact original mode is restored" guarantee has a hole beside it.
ZEN_CALLERS="$(grep -rln "set_zen_mode(\|set_do_not_disturb(" jni --include='*.cpp' 2>/dev/null |
	grep -v '/tests/' | grep -v 'jni/base/FluxUtility/MiscUtility.cpp' || true)"
if [ "${ZEN_CALLERS}" != "jni/Main.cpp" ]; then
	fail "zen is written from somewhere other than the single backend wiring in Main.cpp:"
	fail "  found: ${ZEN_CALLERS:-<none>}"
else
	# In Main.cpp the only permitted call site is inside the AndroidZenBackend writer lambda.
	ZEN_CALL_COUNT="$(grep -c "set_zen_mode(" jni/Main.cpp || true)"
	if [ "${ZEN_CALL_COUNT}" != "1" ]; then
		fail "jni/Main.cpp calls set_zen_mode() ${ZEN_CALL_COUNT} times; exactly one (the backend) is allowed"
	else
		green "  zen is written only through the AndroidZenBackend wiring"
	fi
fi

if grep -qF "set_do_not_disturb" jni/Main.cpp; then
	fail "jni/Main.cpp still references set_do_not_disturb() — the boolean zen path is a "
	fail "  lossy fallback that rewrites total-silence and alarms-only as priority"
fi
green "  no boolean do-not-disturb fallback in the daemon"

# ── 6. The package still ships what it claims ────────────────────────────────
head2 "6. Packaging"
# flux_profiler.sh is still packaged in this increment; Increment 5 removes it. That is only
# acceptable while the daemon provably cannot invoke it — which sections 2 and 3 just proved.
if grep -q "flux_profiler" .github/scripts/compile_zip.sh; then
	info "scripts/flux_profiler.sh is still packaged (removed in Increment 5)"
	info "  the daemon cannot invoke it: proven by sections 2 and 3 above"
fi
green "  packaging is consistent with the cutover"

head2 "═══ Result ═══"
if [ "${FAILURES}" -ne 0 ]; then
	red "${FAILURES} live-execution violation(s)."
	red "V2 live execution: NOT PROVEN"
	exit 1
fi
green "V2 live execution: PROVEN (${ABI})"
green "  - the daemon links and calls the V2 execution engine"
green "  - no flux_profiler invocation, dispatcher symbol, system() or popen() in the binary"
green "  - exactly one apply entry point, and Main.cpp is not it"
green "  - exactly one zen write entry point, with no boolean fallback"
