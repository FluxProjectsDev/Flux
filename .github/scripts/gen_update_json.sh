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
# Generate update.json, the update manifest read by Magisk / KernelSU / APatch managers.
#
# The manager polls the URL in module.prop's `updateJson` field, compares `versionCode` against
# the installed module, and — when it is higher — offers to download `zipUrl` and install it
# directly, without the user visiting GitHub.
#
# The manifest is published as a release asset and the manager is pointed at
#   https://github.com/<repo>/releases/download/<tag>/update.json  (via releases/latest/...)
# so the newest release always describes itself. Nothing is committed back to the repository.
#
# The version/versionCode formulas here must stay identical to compile_zip.sh, which stamps the
# same values into module.prop; the manager compares the two, so a mismatch would either hide a
# real update or offer one that is already installed.
#
# Usage: gen_update_json.sh <tag> <zip-name> [output]

set -euo pipefail

TAG="${1:-}"
ZIP_NAME="${2:-}"
OUT="${3:-update.json}"

if [ -z "$TAG" ] || [ -z "$ZIP_NAME" ]; then
	echo "usage: $0 <tag> <zip-name> [output]" >&2
	exit 1
fi

if [ -z "${GITHUB_REPOSITORY:-}" ]; then
	echo "error: GITHUB_REPOSITORY is not set (this script runs in GitHub Actions)" >&2
	exit 1
fi

version="$(cat version)"
version_code="$(git rev-list HEAD --count)"
release_code="$version_code-$(git rev-parse --short HEAD)-release"

# The changelog is pinned to this release rather than served from a branch. A branch URL would
# keep changing after publication, so a user on an older build could be shown notes that do not
# describe the version they are about to install.
cat >"$OUT" <<EOF
{
  "version": "$version ($release_code)",
  "versionCode": $version_code,
  "zipUrl": "https://github.com/$GITHUB_REPOSITORY/releases/download/$TAG/$ZIP_NAME",
  "changelog": "https://github.com/$GITHUB_REPOSITORY/releases/download/$TAG/changelog.md"
}
EOF

echo "Generated $OUT:"
cat "$OUT"
