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
# Payload installation: runtime, telemetry provider, lifecycle scripts, WebUI, configuration.
#
# Every entry below is classified critical or optional, and the classification is a statement
# about what the module can do without it:
#
#   critical  Flux cannot function. Absent or corrupt -> abort, no summary, temp cleaned.
#   optional  a named feature is unavailable and the user is told which. The install continues
#             and reports SUCCESS WITH LIMITATIONS.
#
# The branding assets are the only genuinely optional entries: a manager that does not render
# banners loses nothing by their absence, and a manager that does renders its stock card. The
# daemon, the telemetry provider, the lifecycle scripts and the WebUI are all critical, because
# every one of them is load-bearing for something the module claims to do.

# Files installed to MODPATH from the package root, each verified against its packaged digest.
FLUX_PAYLOAD_CRITICAL="module.prop service.sh uninstall.sh action.sh cleanup.sh synthesiscore.apk integrity_runtime.sh"
FLUX_PAYLOAD_OPTIONAL="banner.webp donate.webp"

flux_install_module_files() {
	for _entry in ${FLUX_PAYLOAD_CRITICAL}; do
		flux_extract_verified "${ZIPFILE}" "${_entry}" "${MODPATH}" critical
	done
	flux_step_ok "$(echo "${FLUX_PAYLOAD_CRITICAL}" | wc -w) payload file(s) verified"

	_optional_missing=""
	for _entry in ${FLUX_PAYLOAD_OPTIONAL}; do
		if ! flux_extract_verified "${ZIPFILE}" "${_entry}" "${MODPATH}" optional; then
			_optional_missing="${_optional_missing} ${_entry}"
		fi
	done
	if [ -n "${_optional_missing}" ]; then
		flux_step_warn "Branding asset(s) unavailable:${_optional_missing}"
		flux_info "The module works normally; the manager shows its default card."
	else
		flux_step_ok "Branding assets verified"
	fi

	# The manager rewrites module.prop's description at runtime to show live status, so the
	# pristine copy is kept for service.sh and cleanup.sh to restore from.
	cp "${MODPATH}/module.prop" "${MODPATH}/module.prop.orig" ||
		flux_abort "Could not stage module.prop.orig" \
			"The module directory is not writable."
	return 0
}

# The daemon ships under libs/<abi>/ and is installed into system/bin. flux_utility ships already
# placed, so only the ABI-specific binary is selected here.
flux_install_runtime() {
	_abi_dir="${FLUX_ABI_DIR}"
	flux_extract_verified "${ZIPFILE}" "libs/${_abi_dir}/fluxd" "${TMPDIR}" critical

	mkdir -p "${MODPATH}/system/bin"
	cp "${TMPDIR}/libs/${_abi_dir}/fluxd" "${MODPATH}/system/bin/fluxd" ||
		flux_abort "Could not install the Flux runtime" \
			"Copying fluxd into the module directory failed."
	rm -rf "${TMPDIR}/libs"

	# Assert the post-condition rather than assume the copy did what it was told.
	[ -s "${MODPATH}/system/bin/fluxd" ] ||
		flux_abort "The installed runtime is missing or empty" \
			"Expected: ${MODPATH}/system/bin/fluxd"
	flux_step_ok "Runtime installed for ${_abi_dir}"

	flux_extract_verified "${ZIPFILE}" "system/bin/flux_utility" "${MODPATH}" critical
	flux_step_ok "Diagnostics utility installed"
	return 0
}

flux_install_webui() {
	flux_extract_tree "${ZIPFILE}" "webroot/*" "${MODPATH}"
	# The entry point is the thing a manager actually opens; a webroot that extracted without it
	# is a WebUI button that opens a blank page.
	[ -s "${MODPATH}/webroot/index.html" ] ||
		flux_abort "The WebUI failed to install" \
			"webroot/index.html is missing from the installed module." \
			"Re-download Flux and flash it again."

	_count="$(find "${MODPATH}/webroot" -type f 2>/dev/null | wc -l)"
	flux_step_ok "WebUI installed and verified (${_count})"

	# No WebUI icon is checked for: module.prop sets no `webuiIcon`, because no official Flux
	# emblem exists and the manager's own default is the honest fallback. Warning about a missing
	# icon here would report a deliberate choice as a defect on every single install.
	return 0
}

# Configuration is unpacked into a staging directory first and moved into place by the migration
# stage, which is the only code that decides what happens to a pre-existing configuration. Doing
# it in one step here would overwrite the user's settings before anything had classified them.
FLUX_CONFIG_STAGE="${TMPDIR}/.flux-config-stage"

flux_stage_config() {
	rm -rf "${FLUX_CONFIG_STAGE}"
	mkdir -p "${FLUX_CONFIG_STAGE}"
	flux_extract_tree "${ZIPFILE}" "config/*" "${FLUX_CONFIG_STAGE}"

	if [ ! -d "${FLUX_CONFIG_STAGE}/config" ]; then
		flux_abort "The package ships no configuration payload" \
			"config/ is missing from the ZIP."
	fi
	_count="$(find "${FLUX_CONFIG_STAGE}/config" -type f 2>/dev/null | wc -l)"
	[ "${_count}" -gt 0 ] ||
		flux_abort "The package's configuration payload is empty" \
			"config/ contains no files."
	flux_step_ok "Default config staged (${_count} file(s))"
	return 0
}
