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
# Secret scan over the COMMITTED source tree.
#
# verify-release-readiness.sh scans the produced package; this scans what is committed, so a token
# pasted into a source file is caught before it is ever built into an artifact — and, because git
# history keeps it, before it can be quietly removed and forgotten.
#
# It never prints a matched value. A leak reprinted into a public CI log is a second leak, so a
# finding reports the file, the line number and the RULE NAME — never the secret itself.
#
# A deliberately fake fixture may carry the marker  flux-allow-secret  on the same line to opt out;
# the marker is meant to be rare and reviewed. Binary files are skipped (grep -I); this looks for
# secrets in text, and the package scan already covers binaries.
#
# Usage: scan-secrets.sh            # scans tracked files (git ls-files)
#        scan-secrets.sh <path...>  # scans the given paths instead
#
# Exit codes: 0 clean; 1 a secret-shaped string or sensitive file was found.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

red() { printf '\033[31m%s\033[0m\n' "$1"; }
green() { printf '\033[32m%s\033[0m\n' "$1"; }
info() { printf '\033[36m•\033[0m %s\n' "$1"; }
head2() { printf '\n\033[1m%s\033[0m\n' "$1"; }

FINDINGS=0
# report <rule> <file> <line> — names the rule and location, NEVER the value.
report() {
	red "FINDING [$1]: $2:$3"
	FINDINGS=$((FINDINGS + 1))
}

ALLOW_MARKER='flux-allow-secret'

# Rules: "name|ERE". The ERE describes the SHAPE of a credential precisely enough that a match is
# almost certainly real. Loose rules (a bare word like "password") belong in the scoped-assignment
# pass below, not here, because here every match is treated as a leak.
RULES=(
	'github-pat-classic|ghp_[A-Za-z0-9]{36}'
	'github-pat-fine|github_pat_[A-Za-z0-9_]{22,}'
	'github-oauth|gho_[A-Za-z0-9]{36}'
	'github-server|ghs_[A-Za-z0-9]{36}'
	'github-refresh|ghr_[A-Za-z0-9]{36}'
	'telegram-bot-token|[0-9]{8,10}:AA[A-Za-z0-9_-]{33}'
	'private-key-block|-----BEGIN [A-Z ]*PRIVATE KEY-----'
	'aws-access-key-id|AKIA[0-9A-Z]{16}'
	'google-api-key|AIza[0-9A-Za-z_-]{35}'
	'slack-token|xox[baprs]-[0-9A-Za-z-]{10,}'
	'crowdin-token|[Cc]rowdin[^\n]{0,40}[A-Za-z0-9]{40,}'
)

# ── Build the file list ───────────────────────────────────────────────────────
if [ "$#" -gt 0 ]; then
	FILES="$(printf '%s\n' "$@")"
else
	FILES="$(git ls-files)"
fi
FILE_COUNT="$(printf '%s\n' "${FILES}" | grep -c . || true)"
info "scanning ${FILE_COUNT} tracked path(s)"

# FINDINGS is incremented inside subshells (a while in a pipe cannot write the parent's variable),
# so matches are collected into a tally file and reported from the parent afterwards.
TALLY="$(mktemp)"
trap 'rm -f "${TALLY}"' EXIT

# match_into_tally <ere> <case-insensitive:0|1> — appends "<file>\t<line>" per hit to the tally.
# grep -I skips binaries; -n line numbers; -H filename even for one file. The allow marker on the
# same line opts a reviewed fake fixture out. `xargs grep` exits 123 when a batch matches nothing,
# which under `set -o pipefail` would abort the script, so the pipeline is terminated with `|| true`.
match_into_tally() {
	_ere="$1"
	_ci="${2:-0}"
	_flags="-InHaE"
	[ "${_ci}" = "1" ] && _flags="-InHaiE"
	printf '%s\n' "${FILES}" | grep -v '^$' |
		xargs -d '\n' -r grep "${_flags}" -- "${_ere}" 2>/dev/null |
		grep -v -- "${ALLOW_MARKER}" |
		while IFS= read -r _hit; do
			_file="${_hit%%:*}"
			_rest="${_hit#*:}"
			_line="${_rest%%:*}"
			printf '%s\t%s\n' "${_file}" "${_line}" >>"${TALLY}"
		done || true
}

head2 "1. Credential shapes"
: >"${TALLY}"
for rule in "${RULES[@]}"; do
	_name="${rule%%|*}"
	_ere="${rule#*|}"
	: >"${TALLY}.rule"
	match_into_tally "${_ere}" 0
	# Tag each hit for this rule with the rule name, then fold into the master tally.
	if [ -s "${TALLY}" ]; then
		while IFS="$(printf '\t')" read -r _f _l; do
			printf '%s\t%s\t%s\n' "${_name}" "${_f}" "${_l}" >>"${TALLY}.rule"
		done <"${TALLY}"
	fi
	cat "${TALLY}.rule" >>"${TALLY}.all" 2>/dev/null || true
	: >"${TALLY}"
done
if [ -s "${TALLY}.all" ]; then
	while IFS="$(printf '\t')" read -r _n _f _l; do
		report "${_n}" "${_f}" "${_l}"
	done <"${TALLY}.all"
else
	green "  no credential-shaped string in the tracked tree"
fi
rm -f "${TALLY}.all" "${TALLY}.rule"

# ── 2. Sensitive assignments ──────────────────────────────────────────────────
head2 "2. Sensitive assignments"
# A secret assigned to an obviously-named variable with a quoted literal value. Scoped tightly —
# a named key AND a quoted value of real length — so it does not fire on `token = parse(x)` or a
# doc comment. The allow marker still applies.
ASSIGN_ERE='(password|passwd|secret|api[_-]?key|access[_-]?token|private[_-]?key|client[_-]?secret)[[:space:]]*[:=][[:space:]]*("|'"'"')[^"'"'"']{8,}("|'"'"')'
: >"${TALLY}"
match_into_tally "${ASSIGN_ERE}" 1
if [ -s "${TALLY}" ]; then
	while IFS="$(printf '\t')" read -r _f _l; do
		report "hardcoded-credential-assignment" "${_f}" "${_l}"
	done <"${TALLY}"
else
	green "  no hardcoded credential assignment"
fi

# ── 3. Sensitive files committed ──────────────────────────────────────────────
head2 "3. Sensitive files"
SENSITIVE_BEFORE="${FINDINGS}"
while IFS= read -r f; do
	[ -n "${f}" ] || continue
	case "${f}" in
	*.pem | *.key | *.jks | *.keystore | *.p12 | *.pfx | *id_rsa | *id_dsa | *id_ecdsa | *id_ed25519 | *.asc)
		report "committed-key-material" "${f}" "0"
		;;
	*.env)
		# A .env with any `KEY=value` (a value present, not just `KEY=`) is a committed environment
		# file with contents. Empty templates are allowed.
		if grep -qE '^[A-Za-z_][A-Za-z0-9_]*=.+' "${f}" 2>/dev/null; then
			report "committed-dotenv-with-values" "${f}" "0"
		fi
		;;
	esac
done <<<"${FILES}"
[ "${FINDINGS}" -eq "${SENSITIVE_BEFORE}" ] && green "  no committed key material or populated .env"

head2 "═══ Result ═══"
if [ "${FINDINGS}" -ne 0 ]; then
	red "${FINDINGS} secret-scan finding(s)."
	red "A deliberately fake fixture may carry '${ALLOW_MARKER}' on its line to opt out."
	exit 1
fi
green "Secret scan: CLEAN"
green "  - no credential-shaped strings, hardcoded credential assignments, or committed key material"
