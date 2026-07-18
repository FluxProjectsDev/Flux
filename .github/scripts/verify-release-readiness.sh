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
# Stage 2 release-readiness proof: the generated artifact, not the source that intends it.
#
# Everything here reads the produced ZIP and the module.prop *inside it*. The distinction
# matters: compile_zip.sh's intent and the package's contents are different facts, and only one
# of them lands on a device. A stale directory, a half-finished sed, or a secret that leaked into
# the build tree are all invisible from the source and obvious from the artifact.
#
# Sections:
#   1. deterministic inventory (path, type, size, mode, sha256, owner, required/optional)
#   2. generated module.prop contract
#   3. no secrets, tokens, keys or developer paths
#   4. no remote code: the WebUI must not fetch a CDN at runtime
#   5. required artifacts present; removed legacy artifacts absent
#
# Writes the inventory to stdout and, when INVENTORY_OUT is set, to that file so the aggregate
# proof job can attach it.

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

ZIP="${1:-}"
if [ -z "${ZIP}" ] || [ ! -f "${ZIP}" ]; then
	red "ERROR: usage: $0 <module.zip>"
	exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
unzip -qo "${ZIP}" -d "${WORK}/pkg"

# ── 1. Deterministic inventory ───────────────────────────────────────────────
head2 "1. Package inventory"
# Sorted by path so two builds of the same tree produce a diffable inventory. The owner column
# is what makes this reviewable rather than merely complete: an entry nobody owns is an entry
# nobody decided to ship.
INVENTORY="${WORK}/inventory.tsv"
printf 'path\ttype\tsize\tmode\tsha256\towner\tclass\n' >"${INVENTORY}"

classify_owner() {
	case "$1" in
	libs/*/fluxd) printf 'flux-daemon (V2 runtime)' ;;
	system/bin/flux_utility) printf 'flux-utility (diagnostics)' ;;
	synthesiscore.apk) printf 'SynthesisCore (telemetry provider)' ;;
	webroot/*) printf 'WebUI' ;;
	module.prop | customize.sh | service.sh | uninstall.sh | action.sh | cleanup.sh | verify.sh)
		printf 'module lifecycle'
		;;
	gamelist.txt | device_mitigation.json | config/*) printf 'configuration data' ;;
	LICENSE | NOTICE.md) printf 'licensing' ;;
	banner.webp) printf 'branding' ;;
	*) printf 'unclassified' ;;
	esac
}

classify_requirement() {
	case "$1" in
	libs/*/fluxd | module.prop | customize.sh | uninstall.sh | service.sh) printf 'required' ;;
	system/bin/flux_utility | synthesiscore.apk | webroot/index.html) printf 'required' ;;
	LICENSE | NOTICE.md) printf 'required (licensing)' ;;
	*) printf 'optional' ;;
	esac
}

UNCLASSIFIED=0
while IFS= read -r file; do
	rel="${file#"${WORK}/pkg/"}"
	size="$(stat -c '%s' "${file}")"
	mode="$(stat -c '%a' "${file}")"
	sha="$(sha256sum "${file}" | cut -d' ' -f1)"
	kind="$(file -b --mime-type "${file}")"
	owner="$(classify_owner "${rel}")"
	req="$(classify_requirement "${rel}")"
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${rel}" "${kind}" "${size}" "${mode}" "${sha}" \
		"${owner}" "${req}" >>"${INVENTORY}"
	[ "${owner}" = "unclassified" ] && UNCLASSIFIED=$((UNCLASSIFIED + 1))
done < <(find "${WORK}/pkg" -type f | sort)

ENTRY_COUNT="$(($(wc -l <"${INVENTORY}") - 1))"
info "entries: ${ENTRY_COUNT} (unclassified: ${UNCLASSIFIED})"
column -t -s'	' "${INVENTORY}" | head -40
[ "${ENTRY_COUNT}" -gt 40 ] && info "... $((ENTRY_COUNT - 40)) more (full inventory in the artifact)"
if [ -n "${INVENTORY_OUT:-}" ]; then
	cp "${INVENTORY}" "${INVENTORY_OUT}"
	info "inventory written to ${INVENTORY_OUT}"
fi
green "  inventory generated: path, type, size, mode, sha256, owner, class"

# ── 2. The generated module.prop ─────────────────────────────────────────────
head2 "2. Generated module.prop"
PROP="${WORK}/pkg/module.prop"
if [ ! -f "${PROP}" ]; then
	fail "the package has no module.prop"
else
	prop_value() { sed -n "s/^$1=//p" "${PROP}" | head -1; }

	# The template ships these blank and compile_zip.sh fills them in. A blank value here means
	# the sed silently did not match — the module would install with no version at all.
	for key in id name version versionCode author description; do
		if [ -z "$(prop_value "${key}")" ]; then
			fail "module.prop: '${key}' is empty in the *generated* file — the template ships it "
			fail "  blank and the build fills it in, so empty means the substitution missed"
		fi
	done

	[ "$(prop_value id)" = "flux" ] || fail "module.prop: id must be 'flux', got '$(prop_value id)'"

	# The product name must be exactly "Flux". Not "Flux Tweaks", not "Flux <anything>": the
	# module identity a package manager displays is the product name, and it has one canonical
	# form. A substring match (*Flux*) would pass "Flux Tweaks", which is the exact drift this
	# check exists to catch, so it is an equality test.
	PROP_NAME="$(prop_value name)"
	if [ "${PROP_NAME}" = "Flux" ]; then
		green "  name: Flux"
	else
		fail "module.prop: name must be exactly 'Flux', got '${PROP_NAME}'"
	fi
	# And the retired name must not reappear anywhere in the generated metadata.
	if grep -q "Flux Tweaks" "${PROP}"; then
		fail "module.prop still carries the retired product name 'Flux Tweaks'"
	fi
	if grep -qiE '(^|[^a-z])encore([^a-z]|$)' "${PROP}"; then
		fail "module.prop carries upstream Encore branding"
	fi

	# versionCode is what the update mechanism compares. A non-numeric value makes every device
	# think it is either always or never up to date.
	VERSION_CODE="$(prop_value versionCode)"
	if ! [[ "${VERSION_CODE}" =~ ^[0-9]+$ ]]; then
		fail "module.prop: versionCode must be numeric, got '${VERSION_CODE}'"
	else
		green "  versionCode: ${VERSION_CODE} (numeric)"
	fi
	green "  version: $(prop_value version)"

	[ "$(prop_value author)" = "FebriCahyaa" ] ||
		fail "module.prop: unexpected author '$(prop_value author)'"

	# No unsubstituted shell or template leftovers: a literal '$version' on a device is a build
	# that half-ran.
	if grep -qE '\$\{?[A-Za-z_]' "${PROP}"; then
		fail "module.prop contains an unsubstituted shell variable"
	fi

	# Duplicate keys: Magisk reads the first, the author usually means the second.
	DUPES="$(cut -d= -f1 "${PROP}" | sort | uniq -d)"
	if [ -n "${DUPES}" ]; then
		fail "module.prop has duplicate key(s): ${DUPES}"
	fi

	if grep -q $'\r' "${PROP}"; then
		fail "module.prop has CRLF line endings; Magisk expects LF"
	fi
	if ! LC_ALL=C grep -qE '^[[:print:][:space:]]*$' "${PROP}"; then
		fail "module.prop contains non-printable bytes"
	fi

	# updateJson points at wherever this build was produced from. It must not name a foreign or
	# stale organisation: a wrong host here silently offers users someone else's build.
	UPDATE_JSON="$(prop_value updateJson)"
	if [ -n "${UPDATE_JSON}" ]; then
		case "${UPDATE_JSON}" in
		https://github.com/*/releases/latest/download/update.json)
			green "  updateJson: ${UPDATE_JSON}"
			;;
		*) fail "module.prop: updateJson is not a recognised release URL: '${UPDATE_JSON}'" ;;
		esac
		if grep -qiE 'rem01|encore' <<<"${UPDATE_JSON}"; then
			fail "module.prop: updateJson points at the upstream project"
		fi
	else
		info "updateJson is empty (no release channel configured for this build)"
	fi
	green "  module.prop contract validated against the generated file"
fi

# ── 3. Secrets, keys and developer paths ─────────────────────────────────────
head2 "3. Secrets and developer paths"
# A packaged secret is not a bug that can be fixed by a later commit: it is published. This runs
# over the extracted tree, including binaries, because a token pasted into a source file ends up
# in .rodata just as readily as in a script.
SECRET_PATTERNS=(
	'ghp_[A-Za-z0-9]{36}'          # GitHub personal access token
	'github_pat_[A-Za-z0-9_]{22,}' # GitHub fine-grained token
	'gho_[A-Za-z0-9]{36}'
	'[0-9]{8,10}:AA[A-Za-z0-9_-]{33}' # Telegram bot token
	'-----BEGIN [A-Z ]*PRIVATE KEY-----'
	'AKIA[0-9A-Z]{16}' # AWS access key id
)
SECRETS_BEFORE="${FAILURES}"
for pattern in "${SECRET_PATTERNS[@]}"; do
	if grep -rIlaE "${pattern}" "${WORK}/pkg" >/dev/null 2>&1; then
		# Deliberately does not echo the match: printing a leaked secret into a public CI log is
		# how a leak becomes a second leak.
		fail "the package contains something matching a credential pattern (${pattern%%[[]*}...)"
	fi
done
[ "${FAILURES}" -eq "${SECRETS_BEFORE}" ] && green "  no credential-shaped content in the package"

# Signing material must never be packaged.
while IFS= read -r found; do
	fail "the package ships signing material: ${found#"${WORK}/pkg/"}"
done < <(find "${WORK}/pkg" -type f \( -name '*.pem' -o -name '*.key' -o -name '*.jks' \
	-o -name '*.keystore' -o -name '*.p12' -o -name 'id_rsa*' -o -name '*.asc' \) 2>/dev/null)
green "  no signing material"

# Developer filesystem paths in shipped text: they leak the build host's layout and are a sign
# something was packaged from a working tree rather than built.
if grep -rIla --exclude='*.apk' -E '/home/[a-z]+/|/Users/[a-z]+/' "${WORK}/pkg" >/dev/null 2>&1; then
	fail "the package contains a developer home path"
fi
green "  no developer home paths in shipped text"

# Build/VCS debris.
for junk in .git .github node_modules obj .DS_Store Thumbs.db; do
	if [ -e "${WORK}/pkg/${junk}" ]; then
		fail "the package ships build/VCS debris: ${junk}"
	fi
done
green "  no build or VCS debris"

# ── 4. No remote code at runtime ─────────────────────────────────────────────
head2 "4. No remote code"
# The WebUI runs as root's local page. A CDN <script> would mean a third party can execute code
# in a root-privileged context on every user's device, and an offline device would break.
if [ -d "${WORK}/pkg/webroot" ]; then
	if grep -rIlaE '<script[^>]+src="https?://' "${WORK}/pkg/webroot" >/dev/null 2>&1; then
		fail "the WebUI loads a remote script — a third party would execute code in a "
		fail "  root-privileged local page, and an offline device would break"
	fi
	if grep -rIlaE 'https?://(cdn|unpkg|jsdelivr)' "${WORK}/pkg/webroot" >/dev/null 2>&1; then
		fail "the WebUI references a CDN"
	fi
	green "  the WebUI ships its assets and fetches no remote code"
else
	fail "the package has no webroot/"
fi

# ── 5. Required present, legacy absent ───────────────────────────────────────
head2 "5. Contents"
# The daemon ships under libs/<abi>/ and customize.sh copies it into system/bin at install time,
# so the ZIP contains libs/<abi>/fluxd, not system/bin/fluxd. Check where it actually is.
ABIS="${ABIS:-$(sed -n 's/^APP_ABI[[:space:]]*:=[[:space:]]*//p' jni/Application.mk)}"
for abi in ${ABIS}; do
	exe="${WORK}/pkg/libs/${abi}/fluxd"
	if [ ! -f "${exe}" ]; then
		fail "missing required artifact: libs/${abi}/fluxd"
	elif ! file -b "${exe}" | grep -q 'ELF'; then
		fail "libs/${abi}/fluxd is not an ELF binary"
	else
		green "  present: libs/${abi}/fluxd ($(file -b "${exe}" | cut -d, -f1-2))"
	fi
done

for required in module.prop customize.sh uninstall.sh service.sh \
	system/bin/flux_utility synthesiscore.apk webroot/index.html LICENSE NOTICE.md; do
	if [ -f "${WORK}/pkg/${required}" ]; then
		green "  present: ${required}"
	else
		fail "missing required artifact: ${required}"
	fi
done

LEGACY_BEFORE="${FAILURES}"
for legacy in system/bin/flux_profiler flux_profiler.sh Profiler.cpp Profiler.hpp; do
	if [ -e "${WORK}/pkg/${legacy}" ]; then
		fail "the package ships a removed legacy artifact: ${legacy}"
	fi
done
[ "${FAILURES}" -eq "${LEGACY_BEFORE}" ] && green "  no removed legacy artifact in the package"

head2 "═══ Result ═══"
if [ "${FAILURES}" -ne 0 ]; then
	red "${FAILURES} release-readiness violation(s)."
	red "Stage 2 release readiness: NOT PROVEN"
	exit 1
fi
green "Stage 2 release readiness: PROVEN"
green "  - deterministic inventory with owner and requirement classification"
green "  - generated module.prop is complete, numeric where required, and unbranded"
green "  - no credentials, signing material, developer paths or build debris"
green "  - no remote code in the root-privileged WebUI"
green "  - every required artifact present; every removed legacy artifact absent"
