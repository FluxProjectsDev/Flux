#!/usr/bin/env bash
#
# Copyright (C) 2024-2026 FebriCahyaa
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
# Fetch the pinned SynthesisCore release asset into prebuilt/, then verify it.
#
# The download is driven entirely by dependencies/synthesiscore.lock: the exact release tag
# and asset name are pinned there, and the bytes are checked against the pinned SHA-256 before
# they are allowed to replace the committed artifact. There is deliberately no "latest" path.
#
# If the verified artifact is already present and matches the lock, no network access happens
# at all — this is the common case in CI and for offline builds.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOCK_FILE="${REPO_ROOT}/dependencies/synthesiscore.lock"
PREBUILT_DIR="${REPO_ROOT}/prebuilt"
PREBUILT="${PREBUILT_DIR}/synthesiscore.apk"

err() { printf '\033[31mERROR:\033[0m %s\n' "$1" >&2; }
info() { printf '\033[36m•\033[0m %s\n' "$1"; }

lock_field() {
	grep -E "^$1=" "${LOCK_FILE}" | head -n1 | cut -d= -f2-
}

sha256_of() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	else
		shasum -a 256 "$1" | cut -d' ' -f1
	fi
}

REPO="$(lock_field repository)"
VERSION="$(lock_field version)"
ASSET="$(lock_field asset)"
EXPECTED_SHA="$(lock_field sha256)"

# Already have the right bytes? Do nothing, touch no network.
if [ -f "${PREBUILT}" ] && [ "$(sha256_of "${PREBUILT}")" = "${EXPECTED_SHA}" ]; then
	info "Pinned artifact already present and verified; no download needed."
	exit 0
fi

if [ "${VERSION}" = "unreleased-prebuilt" ]; then
	err "the lock points at an unreleased local prebuilt (version=${VERSION})."
	err "there is no release to fetch. Build SynthesisCore and use the committed prebuilt,"
	err "or set SYNTHESISCORE_APK to a local build. See dependencies/synthesiscore.lock."
	exit 1
fi

URL="https://github.com/${REPO}/releases/download/${VERSION}/${ASSET}"
info "Fetching ${URL}"

mkdir -p "${PREBUILT_DIR}"
TMP="$(mktemp)"
trap 'rm -f "${TMP}"' EXIT

if ! curl -fsSL --retry 3 -o "${TMP}" "${URL}"; then
	err "download failed from ${URL}"
	exit 1
fi

ACTUAL_SHA="$(sha256_of "${TMP}")"
if [ "${ACTUAL_SHA}" != "${EXPECTED_SHA}" ]; then
	err "checksum mismatch for the downloaded asset"
	err "  expected: ${EXPECTED_SHA}"
	err "  actual:   ${ACTUAL_SHA}"
	err "refusing to install an artifact that does not match the lock."
	exit 1
fi

mv "${TMP}" "${PREBUILT}"
trap - EXIT
info "Installed and verified ${PREBUILT} (sha256 ${ACTUAL_SHA})"
