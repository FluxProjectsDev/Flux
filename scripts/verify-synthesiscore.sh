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
# Verify the SynthesisCore APK against dependencies/synthesiscore.lock.
#
# Resolution order for the artifact:
#   1. $SYNTHESISCORE_APK  — a local development override (see below)
#   2. prebuilt/synthesiscore.apk — the committed artifact
#
# The build must fail if the bytes do not match the pinned SHA-256. It never falls back to a
# "latest" build and never downloads anything implicitly — fetching is a separate, explicit
# step (fetch-synthesiscore.sh).
#
# Development override:
#   SYNTHESISCORE_APK=/path/to/app-release.apk scripts/verify-synthesiscore.sh
# The override is checked for existence, its checksum is computed and reported, and the build
# is clearly marked as using a local development dependency. It does NOT rewrite the lock.
#
# Exit codes:
#   0  artifact verified (and schema compatible, or overridden)
#   1  artifact missing, checksum mismatch, or malformed lock
#   2  checksum OK but the pinned artifact's schema is incompatible with this Flux build

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOCK_FILE="${REPO_ROOT}/dependencies/synthesiscore.lock"
PREBUILT="${REPO_ROOT}/prebuilt/synthesiscore.apk"

err() { printf '\033[31mERROR:\033[0m %s\n' "$1" >&2; }
warn() { printf '\033[33mWARN:\033[0m %s\n' "$1" >&2; }
info() { printf '\033[36m•\033[0m %s\n' "$1"; }

# Read one `key=value` field from the lock, ignoring comments and blank lines.
lock_field() {
	local key="$1"
	local value
	value="$(grep -E "^${key}=" "${LOCK_FILE}" | head -n1 | cut -d= -f2-)"
	if [ -z "${value}" ]; then
		err "lock file is missing required field '${key}'"
		exit 1
	fi
	printf '%s' "${value}"
}

sha256_of() {
	# sha256sum on Linux, shasum -a 256 on macOS/CI runners that lack it.
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	else
		shasum -a 256 "$1" | cut -d' ' -f1
	fi
}

if [ ! -f "${LOCK_FILE}" ]; then
	err "no lock file at ${LOCK_FILE}"
	exit 1
fi

EXPECTED_SHA="$(lock_field sha256)"
SCHEMA_PROVIDED="$(lock_field schema_provided)"
SCHEMA_MIN="$(lock_field schema_min)"
SCHEMA_MAX="$(lock_field schema_max)"

# --- Development override -----------------------------------------------------
if [ -n "${SYNTHESISCORE_APK:-}" ]; then
	if [ ! -f "${SYNTHESISCORE_APK}" ]; then
		err "SYNTHESISCORE_APK is set but '${SYNTHESISCORE_APK}' does not exist"
		exit 1
	fi
	OVERRIDE_SHA="$(sha256_of "${SYNTHESISCORE_APK}")"
	warn "Using LOCAL DEVELOPMENT dependency, not the pinned release."
	info "  path:   ${SYNTHESISCORE_APK}"
	info "  sha256: ${OVERRIDE_SHA}"
	if [ "${OVERRIDE_SHA}" = "${EXPECTED_SHA}" ]; then
		info "  (matches the pinned checksum)"
	else
		info "  (differs from the pinned checksum — expected for a local build)"
	fi
	info "This build is marked as using a local development SynthesisCore."
	exit 0
fi

# --- Pinned artifact ----------------------------------------------------------
if [ ! -f "${PREBUILT}" ]; then
	err "no artifact at ${PREBUILT}"
	err "run scripts/fetch-synthesiscore.sh, or set SYNTHESISCORE_APK for a local build"
	exit 1
fi

ACTUAL_SHA="$(sha256_of "${PREBUILT}")"
if [ "${ACTUAL_SHA}" != "${EXPECTED_SHA}" ]; then
	err "checksum mismatch for ${PREBUILT}"
	err "  expected: ${EXPECTED_SHA}"
	err "  actual:   ${ACTUAL_SHA}"
	err "the committed artifact does not match the lock. Do not ship it."
	exit 1
fi

info "SynthesisCore artifact verified (sha256 ${ACTUAL_SHA})"

# --- Schema compatibility -----------------------------------------------------
if [ "${SCHEMA_PROVIDED}" -lt "${SCHEMA_MIN}" ] || [ "${SCHEMA_PROVIDED}" -gt "${SCHEMA_MAX}" ]; then
	err "schema mismatch: the pinned artifact emits schema ${SCHEMA_PROVIDED}, but this Flux"
	err "build requires schema ${SCHEMA_MIN}..${SCHEMA_MAX}."
	err ""
	err "Remaining release step:"
	err "  1. Build SynthesisCore from its feat/telemetry-contract-v2 branch"
	err "     (./gradlew :app:assembleRelease), sign it, and publish a release."
	err "  2. Run: scripts/update-synthesiscore-lock.sh <tag> <path-to-app-release.apk>"
	err "  3. Commit prebuilt/synthesiscore.apk and dependencies/synthesiscore.lock together."
	exit 2
fi

info "Schema ${SCHEMA_PROVIDED} is within this build's supported range ${SCHEMA_MIN}..${SCHEMA_MAX}"
