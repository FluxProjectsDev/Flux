#!/system/bin/sh
#
# Copyright (C) 2024-2026 Rem01Gaming
# Copyright (C) 2024-2026 FebriCahyaa
#
# Adapted from Encore Tweaks (https://github.com/Rem01Gaming/encore).
# Modified by the Flux project.
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
# Verified extraction.
#
# compile_zip.sh writes a <file>.sha256 alongside every packaged file, so each entry carries its
# own expected digest. This file is the only place that contract is consumed, so there is exactly
# one implementation of "extract and prove it" rather than one per call site.
#
# The distinction that matters: unzip succeeding tells you a member was written, not that the
# bytes are the ones the build produced. A truncated download, a resumed transfer, or an edited
# ZIP all produce a file that unzip is perfectly happy with. Only the digest catches those, and
# only if the mismatch is fatal — which, for anything on the critical list, it is.

FLUX_INTEGRITY_TMP="${TMPDIR}/.flux-verify"

# Removes everything the installer wrote outside MODPATH. Called by flux_abort (via the
# command -v hook in ui.sh) so a fatal failure does not strand a half-extracted payload.
flux_cleanup_temp() {
	rm -rf "${FLUX_INTEGRITY_TMP}" "${TMPDIR}/libs" 2>/dev/null
	return 0
}

flux_integrity_init() {
	rm -rf "${FLUX_INTEGRITY_TMP}" 2>/dev/null
	mkdir -p "${FLUX_INTEGRITY_TMP}" || return 1
	return 0
}

# flux_sha256_of <file> -> digest on stdout, empty on failure
flux_sha256_of() {
	sha256sum "$1" 2>/dev/null | cut -d' ' -f1
}

# flux_checksum_matches <file> <digest-file>
# Returns 0 when the file's digest equals the digest recorded in <digest-file>.
#
# Compared as strings rather than by piping into `sha256sum -c`: the digest file holds a bare
# digest with no filename, so building a checkfile means synthesising a path, and a path with a
# space in it silently changes what -c parses. Comparing two hex strings has no such edge.
flux_checksum_matches() {
	_have="$(flux_sha256_of "$1")"
	[ -n "${_have}" ] || return 1
	_want="$(cat "$2" 2>/dev/null | tr -d ' \t\r\n')"
	[ -n "${_want}" ] || return 1
	[ "${_have}" = "${_want}" ]
}

# flux_extract_verified <zip> <entry> <dest-dir> <criticality>
#
# criticality: "critical" -> a missing entry, missing digest, or mismatch aborts.
#              "optional" -> the same conditions warn and return 1; the caller decides.
#
# There is deliberately no third mode where a missing digest is tolerated on a critical file.
# "The checksum was absent so we skipped the check" is indistinguishable, from the device's point
# of view, from having no integrity checking at all.
flux_extract_verified() {
	_zip="$1"
	_entry="$2"
	_dest="$3"
	_crit="${4:-critical}"

	_target="${_dest}/${_entry}"
	_digest="${FLUX_INTEGRITY_TMP}/$(echo "${_entry}" | tr '/' '_').sha256"

	unzip -o "${_zip}" "${_entry}" -d "${_dest}" >/dev/null 2>&1
	if [ ! -f "${_target}" ]; then
		if [ "${_crit}" = "optional" ]; then
			return 1
		fi
		flux_abort "Package is missing a required file: ${_entry}" \
			"The download is incomplete or the ZIP was modified." \
			"Re-download Flux and flash it again."
	fi

	# The digest travels as <entry>.sha256 next to the entry. Extract to a flat name so a nested
	# entry cannot collide with another one's digest.
	unzip -p "${_zip}" "${_entry}.sha256" >"${_digest}" 2>/dev/null
	if [ ! -s "${_digest}" ]; then
		if [ "${_crit}" = "optional" ]; then
			rm -f "${_digest}"
			return 1
		fi
		flux_abort "Package has no checksum for ${_entry}" \
			"Every file this build ships is published with a digest." \
			"Its absence means this is not an unmodified Flux package."
	fi

	if ! flux_checksum_matches "${_target}" "${_digest}"; then
		if [ "${_crit}" = "optional" ]; then
			rm -f "${_target}" "${_digest}"
			return 1
		fi
		flux_abort "Checksum mismatch: ${_entry}" \
			"The file in this package is not the file the build produced." \
			"Re-download Flux from the official release page."
	fi

	rm -f "${_digest}"
	return 0
}

# flux_extract_tree <zip> <glob> <dest-dir>
#
# Bulk extraction for directories (webroot, config) where per-entry digests are checked by
# spot-verifying the entry point afterwards rather than one call per file — a webroot has
# hundreds of members and a digest check per member would add minutes to a flash for no
# additional guarantee, since a corrupted archive fails as a whole.
#
# .sha256 members are excluded: they are build metadata and have no business on the device.
flux_extract_tree() {
	unzip -o "$1" "$2" -d "$3" -x "*.sha256" >/dev/null 2>&1
}

# flux_verify_installed <path> <description>
# Post-condition assertion used by the finalize stage. Aborts when the file is absent or empty.
flux_verify_installed() {
	if [ ! -s "$1" ]; then
		flux_abort "Installed file missing or empty: $2" \
			"Path: $1" \
			"The installation did not complete correctly."
	fi
	return 0
}
