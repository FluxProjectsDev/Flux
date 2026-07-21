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
# Runtime integrity: the manifest writer and verifier, in one file so the format has a single
# owner. Sourced — never executed. finalize.sh sources it to WRITE the baseline once the installer
# has already checksum-verified every packaged file; service.sh sources it to VERIFY that baseline
# at each boot before the daemon is allowed to write anything.
#
# ## What this defends against, and what it does not
#
# It defends against a critical runtime file being replaced, truncated, symlinked away, turned
# into a directory, or made group/other-writable AFTER install — the shape a non-root app or a
# botched update leaves behind. It does NOT defend against a root actor: anything running as root
# can rewrite the manifest too, and a module manager can disable Flux outright, so root tampering
# is out of scope by construction (the same threat model action.sh's world-writable check states).
#
# ## Extends the existing checksum system, does not replace it
#
# The package already ships a <file>.sha256 beside every packaged file, and the installer verifies
# each critical file against it at extract time (module/installer/integrity.sh). That establishes
# trust at install. This records the digests of the *installed* critical files — the post-install
# layout, one resolved ABI, real device paths — as the verified generation, and re-checks them at
# boot. It is the runtime half of the same chain, not a second checksum scheme.
#
# ## Manifest format
#
#   # comment lines begin with '#'
#   generation <versionCode>
#   <sha256>  <relpath>
#   ...
#
# <relpath> is relative to the module directory (MODPATH at install, MODDIR at boot). There is no
# mode column on purpose: an "unexpected writable mode" is judged against the live filesystem
# (group/other-writable is a finding regardless of what the manifest once recorded), which cannot
# drift and cannot be defeated by editing a recorded mode.
#
# ## Deliberately excluded from the manifest
#
#   module.prop           Flux rewrites its `description` field at boot (set_module_description_status
#                         in Main.cpp), so hashing it would fail on every healthy device — the exact
#                         false positive the design must avoid. Its id/name/versionCode are still
#                         validated at install by finalize.sh.
#   config-dir files      gamelist.json, device_mitigation.json, the profile/state files: these are
#                         mutable runtime state and user-editable settings, not shipped code. They
#                         are validated at install; hashing live settings at boot is meaningless.
#   customize.sh          runs only at install time and is not part of any runtime write path, so
#                         tampering with it after install changes nothing until a re-install, which
#                         re-verifies the whole package from the ZIP anyway. Excluded so the boot
#                         check does not depend on a file the payload step never installs.
#
# POSIX sh only: no `local`, no arrays, no [[ ]], no process substitution. Helper variables are
# `_ri_`-prefixed so they cannot collide with a sourcing script's variables.

# shellcheck disable=SC2034  # FLUX_RI_STATE/CLASS/REASON are outputs read by the sourcing script

# The critical files, relative to the module directory. ONE definition, shared by the writer and
# every caller, so the protected set cannot drift between install and boot. Each line is a file
# whose replacement changes what runs as root or what the WebUI serves.
flux_ri_critical_files() {
	cat <<'EOF'
system/bin/fluxd
system/bin/flux_utility
synthesiscore.apk
service.sh
action.sh
cleanup.sh
uninstall.sh
integrity_runtime.sh
webroot/index.html
EOF
}

# flux_ri_sha256 <file> -> bare digest on stdout, empty on failure.
flux_ri_sha256() {
	sha256sum "$1" 2>/dev/null | cut -d' ' -f1
}

# flux_ri_write_manifest <manifest_path> <generation> <moddir>
#
# Records the verified generation. Called by the installer AFTER every packaged file has been
# checksum-verified, so the digests captured here are known-good by construction. Returns non-zero
# without leaving a partial file if any listed file cannot be hashed — a manifest that silently
# omitted a file would create a hole the boot check could never see.
flux_ri_write_manifest() {
	_ri_manifest="$1"
	_ri_generation="$2"
	_ri_moddir="$3"
	_ri_tmp="${_ri_manifest}.tmp.$$"

	{
		echo "# Flux runtime integrity manifest"
		echo "# Written by the installer. Do not edit."
		echo "generation ${_ri_generation}"
	} >"${_ri_tmp}" || return 1

	flux_ri_critical_files | while IFS= read -r _ri_rel; do
		[ -n "${_ri_rel}" ] || continue
		_ri_path="${_ri_moddir}/${_ri_rel}"
		_ri_digest="$(flux_ri_sha256 "${_ri_path}")"
		if [ -z "${_ri_digest}" ]; then
			echo "MISSING ${_ri_rel}" >&2
			exit 3
		fi
		printf '%s  %s\n' "${_ri_digest}" "${_ri_rel}" >>"${_ri_tmp}"
	done
	# The while ran in a subshell (pipe); its exit 3 is the pipeline status.
	# shellcheck disable=SC2181
	if [ "$?" -ne 0 ]; then
		rm -f "${_ri_tmp}"
		return 1
	fi

	mv -f "${_ri_tmp}" "${_ri_manifest}" || {
		rm -f "${_ri_tmp}"
		return 1
	}
	chmod 600 "${_ri_manifest}" 2>/dev/null
	return 0
}

# flux_ri_verify <manifest_path> <moddir> <current_generation>
#
# Bounded: hashes the fixed critical set once. Sets three globals for the caller and returns a
# status. Never deletes, never writes outside its own two output variables, never reboots.
#
#   FLUX_RI_STATE   ok | failed | ungoverned
#   FLUX_RI_CLASS   ok | missing | mismatch | denied | symlink | wrongtype | writable | generation
#                   | nomanifest
#   FLUX_RI_REASON  bounded one-line human reason (a class and at most one relpath; never a digest)
#
# Return: 0 ok, 1 failed (caller must enter safe no-write), 2 ungoverned (no manifest; caller may
# continue for backward compatibility with a pre-hardening install and should record the state).
flux_ri_verify() {
	_ri_manifest="$1"
	_ri_moddir="$2"
	_ri_curgen="$3"

	FLUX_RI_STATE="ok"
	FLUX_RI_CLASS="ok"
	FLUX_RI_REASON="all critical files verified"

	if [ ! -f "${_ri_manifest}" ]; then
		# No baseline to check against. A pre-hardening install that upgraded in place will get one
		# written on its next install; until then there is nothing to compare, so this is reported
		# and tolerated rather than treated as tampering (which would brick a legitimate upgrade).
		FLUX_RI_STATE="ungoverned"
		FLUX_RI_CLASS="nomanifest"
		FLUX_RI_REASON="no integrity manifest (pre-hardening install)"
		return 2
	fi

	# Generation gate. On a legitimate install/upgrade the installer rewrites the manifest before
	# the next boot, so the generations match. A mismatch means the manifest was not written by the
	# installer that produced the running module — an unsupported package generation.
	_ri_mangen="$(sed -n 's/^generation[[:space:]]*//p' "${_ri_manifest}" 2>/dev/null | head -1)"
	if [ -n "${_ri_curgen}" ] && [ -n "${_ri_mangen}" ] && [ "${_ri_mangen}" != "${_ri_curgen}" ]; then
		FLUX_RI_STATE="failed"
		FLUX_RI_CLASS="generation"
		FLUX_RI_REASON="manifest generation ${_ri_mangen} does not match module ${_ri_curgen}"
		return 1
	fi

	# Verify each manifest entry. First failing file decides the verdict and is named in the reason;
	# the loop stops there because one confirmed critical mismatch is already a safe-no-write, and
	# naming one file keeps the reason bounded and safe to display.
	_ri_result=0
	while IFS= read -r _ri_line; do
		case "${_ri_line}" in
		'#'* | '' | 'generation '*) continue ;;
		esac
		_ri_want="${_ri_line%%  *}"
		_ri_rel="${_ri_line#*  }"
		_ri_path="${_ri_moddir}/${_ri_rel}"

		# Order matters: -L before -e (which follows symlinks), then existence, then type, then
		# writable mode, then readability, then the digest.
		if [ -L "${_ri_path}" ]; then
			FLUX_RI_CLASS="symlink"
			FLUX_RI_REASON="symlink substituted for ${_ri_rel}"
			_ri_result=1
			break
		fi
		if [ ! -e "${_ri_path}" ]; then
			FLUX_RI_CLASS="missing"
			FLUX_RI_REASON="missing critical file ${_ri_rel}"
			_ri_result=1
			break
		fi
		if [ -d "${_ri_path}" ] || [ ! -f "${_ri_path}" ]; then
			FLUX_RI_CLASS="wrongtype"
			FLUX_RI_REASON="not a regular file: ${_ri_rel}"
			_ri_result=1
			break
		fi
		_ri_mode="$(stat -c '%a' "${_ri_path}" 2>/dev/null)"
		case "${_ri_mode}" in
		*[2367])
			FLUX_RI_CLASS="writable"
			FLUX_RI_REASON="group/other-writable: ${_ri_rel} (${_ri_mode})"
			_ri_result=1
			break
			;;
		esac
		if [ ! -r "${_ri_path}" ]; then
			FLUX_RI_CLASS="denied"
			FLUX_RI_REASON="permission denied reading ${_ri_rel}"
			_ri_result=1
			break
		fi
		_ri_have="$(flux_ri_sha256 "${_ri_path}")"
		if [ -z "${_ri_have}" ]; then
			FLUX_RI_CLASS="denied"
			FLUX_RI_REASON="could not hash ${_ri_rel}"
			_ri_result=1
			break
		fi
		if [ "${_ri_have}" != "${_ri_want}" ]; then
			FLUX_RI_CLASS="mismatch"
			FLUX_RI_REASON="checksum mismatch: ${_ri_rel}"
			_ri_result=1
			break
		fi
	done <"${_ri_manifest}"

	if [ "${_ri_result}" -ne 0 ]; then
		FLUX_RI_STATE="failed"
		return 1
	fi
	FLUX_RI_STATE="ok"
	FLUX_RI_CLASS="ok"
	FLUX_RI_REASON="all critical files verified"
	return 0
}
