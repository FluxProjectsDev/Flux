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
# WebUI production build proof.
#
# The WebUI ships as root's local page inside the module. This proves the PRODUCED build directory
# — the bytes that ship, not the source that intends them — carries only presentation assets: no
# source maps, no development/test files, no remote code, and nothing secret-shaped. It also emits
# a deterministic asset inventory so the shipped set is auditable.
#
# Run it on the Vite output after `bun run build`:
#
#   cd webui && bun run build
#   .github/scripts/verify-webui-inventory.sh webui/dist
#
# Exit codes: 0 clean; 1 a violation or a missing/empty build directory.

set -euo pipefail

red() { printf '\033[31m%s\033[0m\n' "$1"; }
green() { printf '\033[32m%s\033[0m\n' "$1"; }
info() { printf '\033[36m•\033[0m %s\n' "$1"; }
head2() { printf '\n\033[1m%s\033[0m\n' "$1"; }

FAILURES=0
fail() {
	red "FAIL: $1"
	FAILURES=$((FAILURES + 1))
}

DIST="${1:-webui/dist}"
if [ ! -d "${DIST}" ]; then
	red "ERROR: no build directory at ${DIST} — run 'bun run build' in webui/ first."
	exit 1
fi

# ── 1. Entry point present ────────────────────────────────────────────────────
head2 "1. Entry point"
if [ -s "${DIST}/index.html" ]; then
	green "  index.html present"
else
	fail "no index.html in ${DIST} — the WebUI would open a blank page"
fi

# ── 2. No source maps ─────────────────────────────────────────────────────────
head2 "2. No source maps"
# A .map republishes the original source and layout beside the minified bundle. Two shapes: a
# standalone .map file, and an inline sourceMappingURL comment embedded in a shipped asset.
MAPS="$(find "${DIST}" -type f -name '*.map' 2>/dev/null || true)"
if [ -n "${MAPS}" ]; then
	fail "the build ships source map file(s):"
	printf '%s\n' "${MAPS}" | sed 's/^/    /' >&2
else
	green "  no .map files"
fi
if grep -rIla --include='*.js' --include='*.css' 'sourceMappingURL=' "${DIST}" >/dev/null 2>&1; then
	fail "a shipped asset carries an inline sourceMappingURL"
else
	green "  no inline sourceMappingURL"
fi

# ── 3. No development or test artifacts ───────────────────────────────────────
head2 "3. No development / test artifacts"
DEV_BEFORE="${FAILURES}"
# Config, lockfiles, dotenv, and test scaffolding have no place in the shipped output. These are
# build inputs; their presence in dist/ means the build emitted something it should not have.
while IFS= read -r found; do
	[ -n "${found}" ] || continue
	fail "development/test artifact in the build: ${found#"${DIST}/"}"
done < <(find "${DIST}" -type f \( \
	-name 'vite.config.*' -o -name '*.test.*' -o -name '*.spec.*' \
	-o -name '.env' -o -name '.env.*' -o -name 'tsconfig*.json' \
	-o -name 'bun.lock' -o -name 'package-lock.json' -o -name 'yarn.lock' \
	\) 2>/dev/null)
for d in __tests__ tests cypress node_modules .vite; do
	[ -e "${DIST}/${d}" ] && fail "development directory in the build: ${d}"
done
# eruda is the on-screen mobile debug console. It is wired behind VITE_ENABLE_ERUDA (see
# webui/src/main.js) and must NEVER be in a release build: it exposes an in-page eval console on a
# root-privileged page. `bun run build` (no eruda) is the release script; `build:eruda` is not.
if grep -rIla 'eruda' "${DIST}" >/dev/null 2>&1; then
	fail "the build bundles eruda — the debug console must not ship in a release"
fi
[ "${FAILURES}" -eq "${DEV_BEFORE}" ] && green "  no config, lockfiles, dotenv, test scaffolding or eruda"

# ── 4. No remote code ─────────────────────────────────────────────────────────
head2 "4. No remote code"
# The WebUI must ship every asset it needs. A remote <script> would let a third party run code in
# a root-privileged local page, and an offline device would break. Mirrors the package-level check
# in verify-release-readiness.sh, applied to the build directory directly.
REMOTE_BEFORE="${FAILURES}"
if grep -rIlaE '<script[^>]+src="https?://' "${DIST}" >/dev/null 2>&1; then
	fail "a page loads a remote <script>"
fi
if grep -rIlaE 'https?://(cdn|unpkg|jsdelivr|cdnjs)' "${DIST}" >/dev/null 2>&1; then
	fail "an asset references a CDN host"
fi
if grep -rIlaE '\bimport\(["'\''"]https?://' "${DIST}" >/dev/null 2>&1; then
	fail "an asset performs a dynamic import from a URL"
fi
[ "${FAILURES}" -eq "${REMOTE_BEFORE}" ] && green "  the build fetches no remote code"

# ── 5. No secret-shaped values ────────────────────────────────────────────────
head2 "5. No secrets"
# The same credential shapes verify-release-readiness.sh scans the package for, applied to the
# build. A token pasted into a .env or a source file lands in the bundle just as readily.
SECRET_PATTERNS=(
	'ghp_[A-Za-z0-9]{36}'
	'github_pat_[A-Za-z0-9_]{22,}'
	'gho_[A-Za-z0-9]{36}'
	'[0-9]{8,10}:AA[A-Za-z0-9_-]{33}'
	'-----BEGIN [A-Z ]*PRIVATE KEY-----'
	'AKIA[0-9A-Z]{16}'
)
SECRETS_BEFORE="${FAILURES}"
for pattern in "${SECRET_PATTERNS[@]}"; do
	if grep -rIlaE "${pattern}" "${DIST}" >/dev/null 2>&1; then
		# Never echo the match: printing a leaked secret into a public CI log is a second leak.
		fail "the build contains something matching a credential pattern (${pattern%%[[]*}...)"
	fi
done
[ "${FAILURES}" -eq "${SECRETS_BEFORE}" ] && green "  no credential-shaped content in the build"

# ── 6. Deterministic asset inventory ──────────────────────────────────────────
head2 "6. Production asset inventory"
INVENTORY="${WEBUI_INVENTORY_OUT:-webui-inventory.tsv}"
printf 'path\tsize\tsha256\n' >"${INVENTORY}"
while IFS= read -r file; do
	rel="${file#"${DIST}/"}"
	size="$(stat -c '%s' "${file}")"
	sha="$(sha256sum "${file}" | cut -d' ' -f1)"
	printf '%s\t%s\t%s\n' "${rel}" "${size}" "${sha}" >>"${INVENTORY}"
done < <(find "${DIST}" -type f | sort)
ENTRY_COUNT="$(($(wc -l <"${INVENTORY}") - 1))"
info "inventory: ${ENTRY_COUNT} asset(s) → ${INVENTORY}"
head -n 21 "${INVENTORY}" | column -t -s"$(printf '\t')"
[ "${ENTRY_COUNT}" -gt 20 ] && info "... $((ENTRY_COUNT - 20)) more (full inventory in ${INVENTORY})"
green "  deterministic inventory generated"

head2 "═══ Result ═══"
if [ "${FAILURES}" -ne 0 ]; then
	red "${FAILURES} WebUI production violation(s)."
	red "WebUI release inventory: NOT PROVEN"
	exit 1
fi
green "WebUI release inventory: PROVEN"
green "  - entry point present; no source maps (file or inline)"
green "  - no config, lockfiles, dotenv, test scaffolding, eruda or dev directories"
green "  - no remote script, CDN reference or URL import"
green "  - no credential-shaped content"
green "  - deterministic asset inventory emitted"
