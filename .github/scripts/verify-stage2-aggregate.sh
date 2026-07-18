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
# Stage 2 aggregate proof.
#
# This is NOT a job-name checker. A job-name checker passes when other jobs are green, which
# means it passes when someone renames a job away, or when a guarantee is quietly dropped from a
# workflow. This runs as the last step of the proof job, after every individual proof step in the
# same job has already run and (because those steps fail the run on any violation) already
# passed. Reaching this line is itself evidence that they passed — there is no path here that
# skips them.
#
# What it adds on top of that: it re-validates the concrete artifacts those steps produced, so a
# step that "succeeded" while producing nothing is caught, and it writes the human-readable
# summary a reviewer uses to decide the PR is ready. The binaries and the inventory are facts on
# disk; this asserts they say what the run claims.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

red() { printf '\033[31m%s\033[0m\n' "$1"; }
green() { printf '\033[32m%s\033[0m\n' "$1"; }
head2() { printf '\n\033[1m%s\033[0m\n' "$1"; }

FAILURES=0
fail() {
	red "FAIL: $1"
	FAILURES=$((FAILURES + 1))
}

# ── Both production binaries exist and carry the V2 linkage ──────────────────
head2 "Both ABI binaries"
# The link proof ran per ABI earlier in this job. Here we confirm the binaries it inspected are
# both actually present — a job that built only one ABI and skipped the other's proof step would
# otherwise look complete.
NM="${NM:-llvm-nm}"
command -v "${NM}" >/dev/null 2>&1 || NM="nm"
for abi in arm64-v8a armeabi-v7a; do
	exe="obj/local/${abi}/fluxd"
	if [ ! -f "${exe}" ]; then
		fail "${exe} was not built — the aggregate cannot claim both ABIs"
		continue
	fi
	syms="$("${NM}" -C "${exe}" 2>/dev/null || true)"
	if grep -qF "flux::execution::ExecutionEngine::apply" <<<"${syms}"; then
		green "  ${abi}: fluxd links the V2 ExecutionEngine"
	else
		fail "${abi}: fluxd does not link ExecutionEngine::apply"
	fi
done

# ── The package inventory was actually produced ──────────────────────────────
head2 "Package inventory report"
INVENTORY="${INVENTORY:-package-inventory.tsv}"
if [ ! -s "${INVENTORY}" ]; then
	fail "the readiness step reported success but produced no inventory (${INVENTORY}) — a "
	fail "  proof that leaves no artifact cannot be audited after the fact"
else
	ENTRIES="$(($(wc -l <"${INVENTORY}") - 1))"
	# The daemon must be in the inventory. If it is not, the package validated is not a package
	# that ships Flux.
	# The daemon ships at libs/<abi>/fluxd in the ZIP (customize.sh installs it into system/bin).
	if grep -qE "libs/.*/fluxd" "${INVENTORY}"; then
		green "  inventory has ${ENTRIES} entries and includes the daemon"
	else
		fail "the inventory does not list the fluxd daemon"
	fi
	# The legacy applier must not be.
	if grep -qE "(^|/)flux_profiler" "${INVENTORY}"; then
		fail "the inventory lists a legacy flux_profiler artifact"
	else
		green "  inventory lists no legacy applier"
	fi
fi

# ── Release and Telegram remain publication-free ─────────────────────────────
head2 "No publication"
# Check the real condition, not a proxy for it. The risk is that a publishing credential reaches
# this verify-only step; the direct evidence of that is the credential being *in the environment*
# at runtime. A source-text scan cannot do this honestly — the scanner has to name the very
# tokens it forbids, so it matches itself (the same self-reference trap as a "does the code call
# system()" text search). The runtime environment has no such problem: a token is present or it
# is not.
#
# The proof workflow passes this step only INVENTORY. If a future edit wired a release or
# Telegram secret into it, one of these would be non-empty and fail here — before it could be
# used — rather than the check passing because the source happens not to spell a tool's name.
for secret in TELEGRAM_BOT_TOKEN TELEGRAM_CHAT_ID GITHUB_TOKEN GH_TOKEN; do
	if [ -n "${!secret:-}" ]; then
		# Never echo the value.
		fail "a publishing credential (${secret}) is present in the aggregate step's environment "
		fail "  — this step must only verify, and must be given nothing it could publish with"
	fi
done
green "  the aggregate step's environment holds no publishing credential"

# ── Summary ──────────────────────────────────────────────────────────────────
head2 "═══ Stage 2 aggregate ═══"
if [ "${FAILURES}" -ne 0 ]; then
	red "${FAILURES} aggregate violation(s)."
	red "Stage 2: NOT READY"
	exit 1
fi

cat <<'SUMMARY'
Stage 2 V2 execution migration — aggregate proof PASSED.

Proven in this job, each by its own step, each failing the run on any violation:
  • host tests (plain, ASan, UBSan) — separate required jobs
  • NDK arm64-v8a and armeabi-v7a built
  • V2 production link proof, both ABIs (verify-live-execution.sh)
  • single write entry, no system()/popen(), no legacy dispatcher (same)
  • legacy source/build/env/zen absence, both ABIs (same)
  • Category A / Category C boundary intact (verify-provenance-boundary.sh)
  • schema-v2 provenance (verify-schema-provenance.sh)
  • package validation (verify-package.sh)
  • release readiness: inventory, module.prop, secrets, remote code
    (verify-release-readiness.sh)
  • module lifecycle: install / upgrade / uninstall (verify-lifecycle.sh)

Publication remains skipped: no release, no tag, no Telegram on pull_request.
SUMMARY
green "Stage 2: aggregate proof PASSED"
