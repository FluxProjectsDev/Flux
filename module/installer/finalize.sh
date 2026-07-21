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
# Manager integration and the final verification pass.
#
# This is the stage that decides whether the installer is allowed to say "installed". Everything
# it asserts is read back off the installed module tree — not remembered from earlier in the run.
# A stage that copied a file and a tree that contains it are different claims, and only the
# second one is what boots.
#
# Every failure here is fatal via flux_abort, which does not return. That is the structural
# reason a critical failure cannot be followed by a success line: the success path is downstream
# of an exit, not behind a flag that could be left unset.

# ── Manager integration ──────────────────────────────────────────────────────
# KernelSU and APatch do not mount the module's system/ into the filesystem the way Magisk does,
# so the binaries are exposed by symlinking them into the manager's own bin directory, which is
# already on PATH. Magisk needs neither marker nor symlink: its mount makes system/bin/fluxd
# reachable directly.
flux_configure_manager_integration() {
	# Skipped by mountify-style setups regardless of manager.
	touch "${MODPATH}/skip_mountify"

	case "${FLUX_MANAGER}" in
	KernelSU | APatch)
		touch "${MODPATH}/skip_mount"
		flux_info "${FLUX_MANAGER}: binaries exposed by symlink"

		_linked=0
		for _dir in ${FLUX_MANAGER_BIN_DIRS}; do
			# Only directories the manager already created. Creating one ourselves would put
			# files somewhere the manager does not know about and will never clean up.
			[ -d "${_dir}" ] || continue
			ln -sf "${FLUX_MODULE_DIR}/system/bin/fluxd" "${_dir}/fluxd"
			ln -sf "${FLUX_MODULE_DIR}/system/bin/flux_utility" "${_dir}/flux_utility"
			[ -L "${_dir}/fluxd" ] && _linked=$((_linked + 1))
		done

		if [ "${_linked}" -gt 0 ]; then
			flux_step_ok "Runtime linked to manager PATH (${_linked})"
		else
			# Not fatal: the module directory is still mounted for the manager's own use, and
			# service.sh invokes the daemon by path. Only the interactive `fluxd` command from a
			# shell is lost, so this is a limitation, not a failure.
			flux_step_warn "No manager PATH dir; fluxd not on \$PATH"
		fi
		;;
	Magisk)
		flux_info "Magisk: mount provides PATH"
		;;
	*)
		flux_info "Unknown manager: relying on the standard module mount"
		;;
	esac
	return 0
}

# ── Final verification ───────────────────────────────────────────────────────
# Reads the generated module.prop out of the installed tree and checks the fields a manager
# actually depends on. A module.prop that lost its id or versionCode installs and then behaves
# unpredictably in the manager's list, which is hard to diagnose from the device.
flux_verify_module_prop() {
	_prop="${MODPATH}/module.prop"
	flux_verify_installed "${_prop}" "module.prop"

	_id="$(sed -n 's/^id=//p' "${_prop}" | head -1)"
	_name="$(sed -n 's/^name=//p' "${_prop}" | head -1)"
	_version="$(sed -n 's/^version=//p' "${_prop}" | head -1)"
	_code="$(sed -n 's/^versionCode=//p' "${_prop}" | head -1)"

	[ "${_id}" = "flux" ] ||
		flux_abort "module.prop declares the wrong module id" \
			"Expected 'flux', found '${_id}'."
	[ "${_name}" = "Flux" ] ||
		flux_abort "module.prop declares the wrong module name" \
			"Expected 'Flux', found '${_name}'."
	[ -n "${_version}" ] ||
		flux_abort "module.prop has no version" \
			"The package was not stamped by the build."
	case "${_code}" in
	'' | *[!0-9]*)
		flux_abort "module.prop versionCode is not numeric" \
			"Found '${_code}'. Module managers compare it as a number."
		;;
	esac

	flux_step_ok "module.prop verified (${_id}, code ${_code})"
	return 0
}

# Asset metadata must not point at files that are not there: a manager that resolves a missing
# banner shows a broken card rather than no card.
flux_verify_asset_metadata() {
	_prop="${MODPATH}/module.prop"
	_dangling=""
	for _key in banner webuiIcon actionIcon donateIcon; do
		_val="$(sed -n "s/^${_key}=//p" "${_prop}" | head -1)"
		[ -n "${_val}" ] || continue
		if [ ! -s "${MODPATH}/${_val}" ]; then
			_dangling="${_dangling} ${_key}=${_val}"
		fi
	done
	if [ -n "${_dangling}" ]; then
		flux_step_warn "module.prop references missing asset(s):${_dangling}"
		flux_info "The manager falls back to its default card artwork."
	else
		flux_step_ok "Asset references resolve"
	fi
	return 0
}

# The critical inventory. Each entry is something the module cannot do without.
flux_verify_installed_tree() {
	flux_verify_installed "${MODPATH}/system/bin/fluxd" "Flux runtime"
	flux_verify_installed "${MODPATH}/system/bin/flux_utility" "diagnostics utility"
	flux_verify_installed "${MODPATH}/service.sh" "service lifecycle entry"
	flux_verify_installed "${MODPATH}/uninstall.sh" "uninstall support"
	flux_verify_installed "${MODPATH}/cleanup.sh" "cleanup support"
	flux_verify_installed "${MODPATH}/synthesiscore.apk" "SynthesisCore telemetry provider"
	flux_verify_installed "${MODPATH}/webroot/index.html" "WebUI entry point"
	flux_verify_installed "${FLUX_CONFIG_DIR}/gamelist.json" "game list"
	flux_step_ok "All critical components present"

	# The legacy shell applier must not be reachable. The V2 ExecutionEngine is the only write
	# path; a runnable copy of the old one on disk is a way back to it.
	if [ -e "${MODPATH}/system/bin/flux_profiler" ]; then
		flux_abort "The legacy profiler is present in the installed module" \
			"Flux V2 applies profiles through the ExecutionEngine." \
			"This package is not a valid Flux build."
	fi
	flux_step_ok "No legacy profiler payload present"
	return 0
}

# Record the runtime integrity baseline: the verified generation service.sh re-checks at each boot.
#
# Runs after every packaged file was checksum-verified during extraction (installer/integrity.sh)
# AND after flux_verify_installed_tree asserted the installed tree is complete, so the digests
# captured here are known-good by construction. This is the runtime half of the same checksum
# chain the installer already enforces, not a second scheme — see module/integrity_runtime.sh for
# the format, the excluded files (module.prop is runtime-mutated) and the threat model.
flux_write_runtime_manifest() {
	# integrity_runtime.sh is a verified module payload (FLUX_PAYLOAD_CRITICAL), installed and
	# checksum-verified by now. Sourcing it — rather than reimplementing the writer here — keeps
	# the manifest format owned by the one file the boot path also uses, so they cannot drift.
	if [ ! -f "${MODPATH}/integrity_runtime.sh" ]; then
		flux_step_warn "Runtime integrity module absent; boot verification will be ungoverned"
		return 0
	fi
	# shellcheck disable=SC1091  # verified payload, sourced by fixed name from the installed tree
	. "${MODPATH}/integrity_runtime.sh"

	_generation="$(sed -n 's/^versionCode=//p' "${MODPATH}/module.prop" | head -1)"
	_manifest="${FLUX_CONFIG_DIR}/integrity.manifest"
	if flux_ri_write_manifest "${_manifest}" "${_generation}" "${MODPATH}"; then
		flux_verify_installed "${_manifest}" "runtime integrity manifest"
		flux_step_ok "Runtime integrity baseline recorded (generation ${_generation})"
	else
		# Non-fatal. The module installs and runs; boot-time verification simply has no baseline,
		# which service.sh reports as 'ungoverned' and tolerates — the same state a pre-hardening
		# upgrade is in. It never claims a verification it could not actually record.
		flux_step_warn "Could not record the runtime integrity baseline"
	fi
	return 0
}

# Installer scratch state must not survive into the installed module.
flux_verify_clean_tree() {
	_junk=""
	for _leftover in module.prop.sha256 verify.sh.sha256 .flux-verify .flux-config-stage; do
		[ -e "${MODPATH}/${_leftover}" ] && _junk="${_junk} ${_leftover}"
	done
	# The digests shipped alongside every packaged file are install-time metadata. Any that were
	# extracted into MODPATH are removed here so the installed tree carries only real content.
	find "${MODPATH}" -name '*.sha256' -type f -exec rm -f {} + 2>/dev/null

	if [ -n "${_junk}" ]; then
		flux_step_warn "Installer scratch files remained:${_junk}"
	else
		flux_step_ok "No scratch files left behind"
	fi
	return 0
}

flux_finalize() {
	flux_verify_module_prop
	flux_verify_installed_tree
	flux_verify_asset_metadata
	flux_verify_clean_tree
	flux_cleanup_temp

	# Reported with flux_info in BOTH branches, deliberately. An unidentified SoC family is not a
	# limitation and must not downgrade the summary: Flux gates every vendor capability behind
	# runtime certification anyway, so an unrecognised family means the runtime uses the same safe
	# generic behavior the summary already promises for any uncertified capability. Nothing the
	# user installed is missing or degraded.
	#
	# It was a flux_step_warn until CI ran these fixtures on an x86 runner, where no family
	# matches, and every clean install came back as SUCCESS WITH LIMITATIONS. That is the failure
	# mode a warning state has to avoid: firing on the normal case teaches people that warnings
	# are noise, and then the real ones are ignored too.
	if flux_identify_soc; then
		flux_info "SoC family detected: ${FLUX_SOC_NAME}"
	else
		flux_info "SoC family not identified; safe generic behavior will be used"
	fi
	# Written for the runtime to read. Detection only: no tuning is applied here, and no vendor
	# capability is promoted by having identified a family.
	echo "${FLUX_SOC}" >"${FLUX_CONFIG_DIR}/soc_recognition"
	flux_verify_installed "${FLUX_CONFIG_DIR}/soc_recognition" "SoC family record"

	# Last: record the verified generation for the boot-time integrity check.
	flux_write_runtime_manifest
	return 0
}
