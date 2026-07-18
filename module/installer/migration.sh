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
# Existing-installation handling: configuration classification, and cleanup of artifacts earlier
# versions left outside the module directory.
#
# The governing rule is that the user's configuration belongs to the user. An upgrade preserves
# it; it does not "refresh" it, and it does not quietly replace a setting with a default because
# the new version would have chosen differently. The only case where defaults are imposed is a
# configuration this build cannot parse, and even then the original is backed up first and the
# user is told where it went.
#
# Deletion here is narrow by construction: every path removed is either a specific named file, or
# a specific named symlink inside a manager directory that already exists. There is no wildcard
# removal and no removal of a directory the installer did not create.

FLUX_CONFIG_BACKUP=""

# Classify what is already on the device.
#   none      no previous configuration
#   valid     parses as the current format
#   malformed present but unparseable
#
# "Valid" is decided by the daemon itself (`fluxd check_gamelist`), not by a JSON check written in
# shell: the daemon is what has to read the file at runtime, so its opinion is the only one that
# predicts whether the install will actually work. A shell-side approximation would accept files
# the daemon later rejects, which converts an install-time failure into a boot-time one.
flux_classify_config() {
	if [ ! -d "${FLUX_CONFIG_DIR}" ]; then
		echo "none"
		return 0
	fi
	if [ ! -f "${FLUX_CONFIG_DIR}/gamelist.json" ]; then
		echo "none"
		return 0
	fi
	if "${MODPATH}/system/bin/fluxd" check_gamelist >/dev/null 2>&1; then
		echo "valid"
		return 0
	fi
	echo "malformed"
	return 0
}

# Install the packaged defaults for any file the user does not already have.
#
# Per-file rather than a directory copy: a directory copy would overwrite the user's settings
# with the shipped defaults on every upgrade, which is precisely the data loss this stage exists
# to prevent. New defaults introduced by a release still land, because a file the user has never
# had is not a file they have customised.
flux_apply_default_config() {
	mkdir -p "${FLUX_CONFIG_DIR}"
	_added=0
	_kept=0
	for _src in "${FLUX_CONFIG_STAGE}"/config/*; do
		[ -e "${_src}" ] || continue
		_name="$(basename "${_src}")"
		if [ -e "${FLUX_CONFIG_DIR}/${_name}" ]; then
			_kept=$((_kept + 1))
		else
			cp -r "${_src}" "${FLUX_CONFIG_DIR}/${_name}" || continue
			_added=$((_added + 1))
		fi
	done
	flux_info "Configuration: ${_kept} existing file(s) preserved, ${_added} new default(s) added"
	return 0
}

# Back up a configuration this build cannot read, then let the defaults apply.
#
# Backed up rather than deleted: it is the user's data, it may be the only record of settings
# they spent time on, and a malformed file is often trivially repairable by hand. The backup name
# is fixed rather than timestamped so repeated failed upgrades cannot accumulate copies without
# bound.
flux_backup_malformed_config() {
	FLUX_CONFIG_BACKUP="${FLUX_CONFIG_DIR}/gamelist.json.invalid"
	if [ -f "${FLUX_CONFIG_DIR}/gamelist.json" ]; then
		mv -f "${FLUX_CONFIG_DIR}/gamelist.json" "${FLUX_CONFIG_BACKUP}" 2>/dev/null || {
			rm -f "${FLUX_CONFIG_DIR}/gamelist.json"
			FLUX_CONFIG_BACKUP=""
		}
	fi
	return 0
}

# Regenerate the gamelist from the packaged source. Fatal on failure: the daemon reads this file
# at startup, and a module that installs "successfully" and then cannot start is a worse outcome
# than one that refuses to install.
flux_generate_gamelist() {
	flux_extract_verified "${ZIPFILE}" "gamelist.txt" "${FLUX_CONFIG_DIR}" critical
	"${MODPATH}/system/bin/fluxd" setup_gamelist "${FLUX_CONFIG_DIR}/gamelist.txt"
	_rc=$?
	rm -f "${FLUX_CONFIG_DIR}/gamelist.txt"
	if [ "${_rc}" -ne 0 ]; then
		flux_abort "Could not initialise the game list" \
			"fluxd setup_gamelist exited with status ${_rc}." \
			"Flux cannot start without it."
	fi
	[ -s "${FLUX_CONFIG_DIR}/gamelist.json" ] ||
		flux_abort "The game list was not written" \
			"Expected: ${FLUX_CONFIG_DIR}/gamelist.json"
	return 0
}

# ── Artifacts previous versions left outside the module directory ────────────
# These are the ones replacing the module cannot clean up on its own, because they are not in it.
#
# flux_profiler is the pre-V2 shell applier. Its symlink lives in the manager's bin directory, so
# an upgrade leaves it behind, dangling, and anyone who runs it gets a confusing failure rather
# than "command not found". The applier is gone; its symlink goes with it.
flux_clean_legacy_artifacts() {
	_removed=0
	for _dir in ${FLUX_MANAGER_BIN_DIRS}; do
		[ -d "${_dir}" ] || continue
		if [ -e "${_dir}/flux_profiler" ] || [ -L "${_dir}/flux_profiler" ]; then
			rm -f "${_dir}/flux_profiler" && _removed=$((_removed + 1))
		fi
	done

	# Root-detection mitigation artifacts written by much older builds at fixed paths. Named
	# individually; there is no wildcard here and no directory removal beyond /data/flux, which
	# only ever contained Flux's own state.
	if [ -d /data/flux ]; then
		rm -rf /data/flux && _removed=$((_removed + 1))
	fi
	if [ -f /data/local/tmp/flux_logo.png ]; then
		rm -f /data/local/tmp/flux_logo.png && _removed=$((_removed + 1))
	fi

	if [ "${_removed}" -gt 0 ]; then
		flux_step_ok "Removed ${_removed} stale artifact(s) from earlier Flux versions"
	else
		flux_step_ok "No stale artifacts from earlier versions"
	fi
	return 0
}

# The full existing-installation stage.
flux_migrate_existing() {
	flux_clean_legacy_artifacts

	case "${FLUX_INSTALL_MODE}" in
	clean)
		flux_step_ok "No previous installation detected"
		;;
	incomplete)
		flux_step_warn "A previous installation did not finish; it will be replaced"
		;;
	upgrade)
		flux_step_ok "Existing Flux installation detected; configuration will be preserved"
		;;
	esac
	return 0
}

# Runs after the runtime is installed, because classification needs the daemon to adjudicate.
flux_settle_config() {
	_state="$(flux_classify_config)"
	case "${_state}" in
	valid)
		flux_step_ok "Existing configuration is valid and was preserved"
		flux_apply_default_config
		;;
	malformed)
		flux_backup_malformed_config
		if [ -n "${FLUX_CONFIG_BACKUP}" ]; then
			flux_step_warn "Existing configuration is malformed; backed up and defaults restored"
			flux_info "Backup: ${FLUX_CONFIG_BACKUP}"
		else
			flux_step_warn "Existing configuration is malformed; defaults restored"
		fi
		flux_apply_default_config
		flux_generate_gamelist
		flux_step_ok "Game list regenerated from packaged defaults"
		;;
	none)
		flux_apply_default_config
		flux_generate_gamelist
		flux_step_ok "Game list initialised"
		;;
	esac
	return 0
}
