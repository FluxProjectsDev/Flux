#!/system/bin/sh
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
# Installation environment detection.
#
# Reports which module manager is running the install, on what device, and whether this is a
# clean install or an upgrade.
#
# Detection is INFORMATIVE. Nothing downstream is allowed to skip a validation because a
# particular manager was detected — the checks that matter (ABI, API level, payload integrity,
# post-install verification) run identically everywhere. Manager identity is used for exactly
# three things: choosing where to place PATH symlinks, deciding whether the mount-skip markers
# apply, and telling the user what was detected.
#
# No single environment variable is trusted on its own. KSU=true is set by KernelSU, but APatch
# also sets it for compatibility, so testing KSU first would report every APatch install as
# KernelSU. APatch is therefore tested first, on its own dedicated variable.

FLUX_MANAGER="unknown"
FLUX_MANAGER_VERSION=""
FLUX_ABI=""
FLUX_ABI_DIR=""
FLUX_API=""
FLUX_INSTALL_MODE="clean"
# These three default to the real device paths and are only ever overridden by the CI lifecycle
# fixtures, which redirect them into a temp root so a test run cannot touch a real /data/adb.
# Assignment is `${VAR:-default}` rather than a bare literal purely to create that seam: on a
# device nothing sets them, so the defaults are what run.
FLUX_MODULE_DIR="${FLUX_MODULE_DIR:-/data/adb/modules/flux}"
# shellcheck disable=SC2034  # read by migration.sh and finalize.sh, which source this file
FLUX_CONFIG_DIR="${FLUX_CONFIG_DIR:-/data/adb/.config/flux}"

# Directories where a manager exposes binaries on $PATH. Only ones that already exist are used;
# the installer never creates a manager's private directory, because guessing a path a manager
# did not create is how a module ends up writing outside anything the manager will clean up.
# shellcheck disable=SC2034  # read by migration.sh and finalize.sh, which source this file
FLUX_MANAGER_BIN_DIRS="${FLUX_MANAGER_BIN_DIRS:-/data/adb/ap/bin /data/adb/ksu/bin}"

flux_detect_manager() {
	# APatch before KernelSU: APatch sets KSU=true for module compatibility, so the KSU test
	# alone cannot distinguish them.
	if [ "${APATCH:-}" = "true" ] || [ -n "${APATCH_VER_CODE:-}" ]; then
		FLUX_MANAGER="APatch"
		FLUX_MANAGER_VERSION="${APATCH_VER:-${APATCH_VER_CODE:-}}"
		return 0
	fi
	if [ "${KSU:-}" = "true" ] || [ -n "${KSU_VER_CODE:-}" ]; then
		FLUX_MANAGER="KernelSU"
		FLUX_MANAGER_VERSION="${KSU_VER:-${KSU_VER_CODE:-}}"
		return 0
	fi
	if [ -n "${MAGISK_VER_CODE:-}" ] || [ -n "${MAGISKTMP:-}" ]; then
		FLUX_MANAGER="Magisk"
		FLUX_MANAGER_VERSION="${MAGISK_VER:-${MAGISK_VER_CODE:-}}"
		return 0
	fi
	FLUX_MANAGER="unknown"
	FLUX_MANAGER_VERSION=""
	return 0
}

# An unrecognised manager is allowed to proceed, but only on evidence that it implements the
# standard module contract this installer depends on. That contract is concrete: the variables
# customize.sh is handed, and the two functions it calls. If any are absent, the install would
# fail partway through with a shell error instead of a message, so it stops here with one.
flux_verify_module_contract() {
	_missing=""
	[ -n "${MODPATH:-}" ] || _missing="${_missing} MODPATH"
	[ -n "${ZIPFILE:-}" ] || _missing="${_missing} ZIPFILE"
	[ -n "${TMPDIR:-}" ] || _missing="${_missing} TMPDIR"
	[ -n "${ARCH:-}" ] || _missing="${_missing} ARCH"
	[ -n "${API:-}" ] || _missing="${_missing} API"
	command -v ui_print >/dev/null 2>&1 || _missing="${_missing} ui_print"
	command -v set_perm_recursive >/dev/null 2>&1 || _missing="${_missing} set_perm_recursive"

	if [ -n "${_missing}" ]; then
		flux_abort "This installer environment is not supported." \
			"The module contract is incomplete. Missing:${_missing}" \
			"Install Flux with Magisk, KernelSU, or APatch."
	fi
	return 0
}

# Upgrade versus clean install.
#
# Determined from the installed module directory, not from the configuration directory: config
# survives an uninstall in some manager versions, so its presence proves a previous install
# existed at some point, not that one is installed now. `module.prop` is the file the manager
# itself requires, so its presence is the closest thing to an authoritative answer available.
flux_detect_install_mode() {
	if [ -f "${FLUX_MODULE_DIR}/module.prop" ]; then
		FLUX_INSTALL_MODE="upgrade"
	elif [ -d "${FLUX_MODULE_DIR}" ] && [ -n "$(ls -A "${FLUX_MODULE_DIR}" 2>/dev/null)" ]; then
		# A NON-EMPTY module directory with no module.prop is a previous install that did not
		# finish. Emptiness is the load-bearing half of that test: managers routinely create the
		# directory before handing off to the installer, so "the directory exists" on its own
		# would report every clean install as a broken one — and that warning would then downgrade
		# a perfectly good install to SUCCESS WITH LIMITATIONS.
		FLUX_INSTALL_MODE="incomplete"
	else
		FLUX_INSTALL_MODE="clean"
	fi
	return 0
}

flux_detect_environment() {
	flux_detect_manager
	flux_verify_module_contract
	flux_detect_install_mode
	FLUX_API="${API}"
	FLUX_ABI="${ARCH}"
	return 0
}

flux_report_environment() {
	if [ -n "${FLUX_MANAGER_VERSION}" ]; then
		flux_info "Manager: ${FLUX_MANAGER} (${FLUX_MANAGER_VERSION})"
	else
		flux_info "Manager: ${FLUX_MANAGER}"
	fi
	flux_info "Android API: ${FLUX_API}"
	flux_info "Architecture: ${FLUX_ABI} -> ${FLUX_ABI_DIR}"
	case "${FLUX_INSTALL_MODE}" in
	upgrade) flux_info "Mode: Flux upgrade" ;;
	incomplete) flux_info "Mode: repairing an incomplete previous install" ;;
	*) flux_info "Mode: clean install" ;;
	esac
	return 0
}
