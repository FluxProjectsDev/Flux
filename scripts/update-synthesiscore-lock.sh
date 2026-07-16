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
# Regenerate dependencies/synthesiscore.lock from a real SynthesisCore build.
#
# Usage:
#   scripts/update-synthesiscore-lock.sh <version-tag> <path-to-apk> [commit-sha] [schema-provided]
#
# Example, after building and releasing SynthesisCore v2.0.0:
#   scripts/update-synthesiscore-lock.sh v2.0.0 \
#       ../SynthesisCore/app/build/outputs/apk/release/app-release.apk \
#       "$(git -C ../SynthesisCore rev-parse HEAD)" 2
#
# This is the one place the lock is allowed to change. It computes the real SHA-256 of the
# real bytes you pass it — it never invents a checksum. It also copies the artifact into
# prebuilt/ so the lock and the committed binary stay in lockstep; commit both together.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOCK_FILE="${REPO_ROOT}/dependencies/synthesiscore.lock"
PREBUILT="${REPO_ROOT}/prebuilt/synthesiscore.apk"

err() { printf '\033[31mERROR:\033[0m %s\n' "$1" >&2; }
info() { printf '\033[36m•\033[0m %s\n' "$1"; }

if [ "$#" -lt 2 ]; then
	err "usage: $0 <version-tag> <path-to-apk> [commit-sha] [schema-provided]"
	exit 1
fi

VERSION="$1"
APK_PATH="$2"
COMMIT="${3:-}"
SCHEMA_PROVIDED="${4:-2}"

if [ ! -f "${APK_PATH}" ]; then
	err "artifact not found: ${APK_PATH}"
	exit 1
fi

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
ASSET="$(lock_field asset)"
SCHEMA_MIN="$(lock_field schema_min)"
SCHEMA_MAX="$(lock_field schema_max)"

if [ -z "${COMMIT}" ]; then
	err "no commit SHA supplied and none could be inferred; pass it explicitly as arg 3"
	exit 1
fi
if ! printf '%s' "${COMMIT}" | grep -Eq '^[0-9a-f]{40}$'; then
	err "commit must be a full 40-character SHA, got: ${COMMIT}"
	exit 1
fi

SHA="$(sha256_of "${APK_PATH}")"

info "Updating lock:"
info "  version:         ${VERSION}"
info "  commit:          ${COMMIT}"
info "  sha256:          ${SHA}"
info "  schema_provided: ${SCHEMA_PROVIDED}"

cp "${APK_PATH}" "${PREBUILT}"
info "Copied artifact to ${PREBUILT}"

# Rewrite only the pinned fields; keep the header comments and the schema range.
tmp="$(mktemp)"
{
	sed -n '1,/^$/p' "${LOCK_FILE}" | sed '/^repository=/,$d'
	cat <<EOF
repository=${REPO}
version=${VERSION}
commit=${COMMIT}
asset=${ASSET}
sha256=${SHA}
schema_provided=${SCHEMA_PROVIDED}
schema_min=${SCHEMA_MIN}
schema_max=${SCHEMA_MAX}
EOF
} >"${tmp}"
mv "${tmp}" "${LOCK_FILE}"

info "Lock updated. Commit prebuilt/synthesiscore.apk and dependencies/synthesiscore.lock together."
