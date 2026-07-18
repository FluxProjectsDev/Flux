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
# Installer trust root.
#
# customize.sh sources this file before anything else, and this file verifies the installer
# helpers before THEY are sourced. That ordering is the point: everything downstream runs as root
# straight out of the ZIP, so the helpers must be checked against their published digests before
# a single line of them executes, not after.
#
# The bootstrap gap is real and worth stating plainly: this file is itself extracted and sourced
# without having been checked, because checking it would require code that is not yet loaded. No
# script inside a package can close that gap — the assurance that the ZIP as a whole is authentic
# comes from where it was downloaded and from the manager's handling of it. What this file does
# close is the much larger gap that existed before: previously every helper, and every payload
# file, was extracted by a `extract()` sourced without verification. Now exactly one file is
# unverified, and it is this one.
#
# Only sha256sum, unzip and shell builtins are used here, so nothing this depends on comes out of
# the package it is checking.

FLUX_INSTALLER_DIR="${TMPDIR}/installer"

# The helpers, in dependency order. config.sh and ui.sh come first because flux_abort lives in
# ui.sh: nothing loaded before it can report a failure the normal way.
FLUX_INSTALLER_HELPERS="config.sh ui.sh integrity.sh environment.sh compatibility.sh payload.sh permissions.sh migration.sh finalize.sh"

# Failure reporting for the window before ui.sh is loaded, so it cannot use flux_abort.
_flux_bootstrap_abort() {
	ui_print "*********************************************************"
	ui_print "! $1"
	ui_print "! $2"
	ui_print "! Installation aborted."
	ui_print "! Re-download Flux from the official release page."
	ui_print "*********************************************************"
	rm -rf "${FLUX_INSTALLER_DIR}" 2>/dev/null
	abort ""
}

flux_bootstrap_installer() {
	rm -rf "${FLUX_INSTALLER_DIR}" 2>/dev/null
	mkdir -p "${FLUX_INSTALLER_DIR}" ||
		_flux_bootstrap_abort "Cannot create the installer work directory." \
			"TMPDIR is not writable."

	unzip -o "${ZIPFILE}" "installer/*" -d "${TMPDIR}" >/dev/null 2>&1

	for _helper in ${FLUX_INSTALLER_HELPERS}; do
		_path="${FLUX_INSTALLER_DIR}/${_helper}"
		_digest="${FLUX_INSTALLER_DIR}/${_helper}.sha256"

		[ -s "${_path}" ] ||
			_flux_bootstrap_abort "Installer component missing: ${_helper}" \
				"The package is incomplete or was modified."
		[ -s "${_digest}" ] ||
			_flux_bootstrap_abort "No checksum for installer component: ${_helper}" \
				"This is not an unmodified Flux package."

		_have="$(sha256sum "${_path}" 2>/dev/null | cut -d' ' -f1)"
		_want="$(tr -d ' \t\r\n' <"${_digest}")"
		if [ -z "${_have}" ] || [ "${_have}" != "${_want}" ]; then
			_flux_bootstrap_abort "Checksum mismatch: installer/${_helper}" \
				"An installer component does not match the published build."
		fi
	done

	# Verified above, so loading is safe. Sourced by explicit name from a fixed list, in a
	# directory this function created: no path is built from input, and nothing is globbed into
	# execution.
	for _helper in ${FLUX_INSTALLER_HELPERS}; do
		# shellcheck disable=SC1090  # path comes from the fixed literal list above
		. "${FLUX_INSTALLER_DIR}/${_helper}"
	done

	rm -f "${FLUX_INSTALLER_DIR}"/*.sha256 2>/dev/null
	return 0
}

# Provenance of the Magisk installer stub.
#
# The update-binary carries a digest when the package was downloaded as a release asset, and does
# not when the Magisk app fetched it through its own update flow. Its absence is therefore
# informational; a MISMATCH is not, and is fatal.
#
# Echoes "verified" or "unsigned" for the caller to report.
flux_check_update_binary() {
	_ub="META-INF/com/google/android/update-binary"
	_work="${TMPDIR}/.flux-ub"
	rm -rf "${_work}" 2>/dev/null
	mkdir -p "${_work}" || return 0

	unzip -o "${ZIPFILE}" "${_ub}" -d "${_work}" >/dev/null 2>&1
	if [ ! -s "${_work}/${_ub}" ]; then
		rm -rf "${_work}"
		echo "absent"
		return 0
	fi

	unzip -p "${ZIPFILE}" "${_ub}.sha256" >"${_work}/ub.sha256" 2>/dev/null
	if [ -s "${_work}/ub.sha256" ]; then
		_have="$(sha256sum "${_work}/${_ub}" 2>/dev/null | cut -d' ' -f1)"
		_want="$(tr -d ' \t\r\n' <"${_work}/ub.sha256")"
		rm -rf "${_work}"
		if [ "${_have}" != "${_want}" ]; then
			flux_abort "Checksum mismatch: ${_ub}" \
				"The installer stub does not match the published build."
		fi
		echo "verified"
		return 0
	fi

	rm -rf "${_work}"
	echo "unsigned"
	return 0
}
