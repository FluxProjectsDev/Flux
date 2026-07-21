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
# Release metadata + checksum manifest for the produced flashable ZIP.
#
# Emits two files a user (or the attestation step) can verify against:
#
#   SHA256SUMS          standard `sha256sum` format over the ZIP and any sibling release assets,
#                       so `sha256sum -c SHA256SUMS` works unmodified.
#   build-metadata.json traceability for the exact artifact: which commit, which workflow run,
#                       which toolchain, which ABIs, and the digests of the ZIP and the bundled
#                       SynthesisCore APK.
#
# Deterministic facts (source commit, ABIs, digests) are kept separate from environment-dependent
# ones (toolchain versions, run identity, timestamp) so a reader can tell what would reproduce
# bit-for-bit from what merely records where this build happened. This does NOT claim bit-for-bit
# reproducibility — that requires independent verification (see docs/security/release-verification.md).
#
# Usage: gen_release_metadata.sh <zip> [output-dir]
# Honours, when set: GITHUB_SHA, GITHUB_REF, GITHUB_WORKFLOW, GITHUB_RUN_ID, GITHUB_RUN_NUMBER,
#                    GITHUB_SERVER_URL, GITHUB_REPOSITORY, RUNNER_OS, NDK_VERSION, GITHUB_OUTPUT.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

ZIP="${1:-}"
OUTDIR="${2:-.}"
if [ -z "${ZIP}" ] || [ ! -f "${ZIP}" ]; then
	echo "usage: $0 <zip> [output-dir]  (zip must exist)" >&2
	exit 1
fi

ZIP_NAME="$(basename "${ZIP}")"
ZIP_SHA="$(sha256sum "${ZIP}" | cut -d' ' -f1)"

# ── SHA256SUMS ────────────────────────────────────────────────────────────────
# Standard format so users run `sha256sum -c SHA256SUMS` with no massaging. Paths are basenames,
# resolved from the directory the ZIP is in, so the manifest verifies wherever the assets sit
# together.
ZIP_DIR="$(cd "$(dirname "${ZIP}")" && pwd)"
SUMS="${OUTDIR}/SHA256SUMS"
: >"${SUMS}"
(
	cd "${ZIP_DIR}"
	for asset in "${ZIP_NAME}" update.json changelog.md; do
		[ -f "${asset}" ] || continue
		sha256sum "${asset}"
	done
) >>"${SUMS}"
echo "Wrote ${SUMS}:"
cat "${SUMS}"

# ── build-metadata.json ───────────────────────────────────────────────────────
ABIS="$(sed -n 's/^APP_ABI[[:space:]]*:=[[:space:]]*//p' jni/Application.mk)"
APK_SHA="$(sed -n 's/^sha256=//p' dependencies/synthesiscore.lock | head -1)"
APK_NAME="$(sed -n 's/^asset=//p' dependencies/synthesiscore.lock | head -1)"
SOURCE_COMMIT="${GITHUB_SHA:-$(git rev-parse HEAD 2>/dev/null || echo unknown)}"
VERSION="$(cat version 2>/dev/null || echo unknown)"

# jq builds the JSON so no value is ever interpolated into a string context unescaped. The two
# top-level objects draw the line §12 asks for: `deterministic` is what a re-build should
# reproduce; `environment` is where and with what this particular build ran.
METADATA="${OUTDIR}/build-metadata.json"
jq -n \
	--arg version "${VERSION}" \
	--arg zip_name "${ZIP_NAME}" \
	--arg zip_sha "${ZIP_SHA}" \
	--arg source_commit "${SOURCE_COMMIT}" \
	--arg abis "${ABIS}" \
	--arg apk_name "${APK_NAME}" \
	--arg apk_sha "${APK_SHA}" \
	--arg ndk "${NDK_VERSION:-unknown}" \
	--arg runner_os "${RUNNER_OS:-unknown}" \
	--arg workflow "${GITHUB_WORKFLOW:-local}" \
	--arg run_id "${GITHUB_RUN_ID:-local}" \
	--arg run_number "${GITHUB_RUN_NUMBER:-0}" \
	--arg server "${GITHUB_SERVER_URL:-https://github.com}" \
	--arg repo "${GITHUB_REPOSITORY:-local}" \
	--arg ref "${GITHUB_REF:-local}" \
	--arg built_at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
	'{
	  schema: 1,
	  version: $version,
	  deterministic: {
	    source_commit: $source_commit,
	    abis: ($abis | split(" ") | map(select(length > 0))),
	    zip_name: $zip_name,
	    zip_sha256: $zip_sha,
	    synthesiscore_apk: { asset: $apk_name, sha256: $apk_sha }
	  },
	  environment: {
	    ndk_version: $ndk,
	    runner_os: $runner_os,
	    workflow: $workflow,
	    run_id: $run_id,
	    run_number: ($run_number | tonumber? // 0),
	    run_url: ($server + "/" + $repo + "/actions/runs/" + $run_id),
	    ref: $ref,
	    built_at: $built_at
	  },
	  note: "Digests are deterministic; toolchain, run identity and timestamp are environment-dependent. Bit-for-bit reproducibility is not claimed until independently verified."
	}' >"${METADATA}"
echo "Wrote ${METADATA}:"
cat "${METADATA}"

# Hand the digest back to the workflow so the attestation step and the subject-digest proof use
# the exact value computed here, not a re-derivation that could disagree.
if [ -n "${GITHUB_OUTPUT:-}" ]; then
	{
		echo "zip_sha256=${ZIP_SHA}"
		echo "sha256sums=${SUMS}"
		echo "metadata=${METADATA}"
	} >>"${GITHUB_OUTPUT}"
fi
