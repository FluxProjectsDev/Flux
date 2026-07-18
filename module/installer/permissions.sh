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
# Ownership and mode.
#
# Applying a mode and verifying it are separate operations here, on purpose. set_perm_recursive
# reports success on a filesystem that silently ignored it — a ZIP extracted onto a mount without
# permission support is the common case — and the failure surfaces at boot as a daemon that never
# starts, with nothing in the install log to explain it. Reading the mode back turns that into an
# install-time abort with a specific cause.

# flux_is_executable <path>
# `test -x` rather than parsing `ls`: it asks the kernel the question that actually matters, and
# it does not depend on ls output formatting or on a busybox applet being present.
flux_is_executable() {
	[ -f "$1" ] && [ -x "$1" ]
}

flux_apply_permissions() {
	set_perm_recursive "${MODPATH}/system/bin" 0 0 0755 0755

	# The lifecycle scripts the manager sources or runs. Modes are applied explicitly rather than
	# inherited from the ZIP, whose stored modes depend on the machine that built it.
	for _script in service.sh uninstall.sh action.sh cleanup.sh; do
		[ -f "${MODPATH}/${_script}" ] || continue
		set_perm "${MODPATH}/${_script}" 0 0 0755
	done

	# The WebUI is read by the manager's WebView as root. Nothing in it is executed, so nothing
	# in it needs an execute bit.
	if [ -d "${MODPATH}/webroot" ]; then
		set_perm_recursive "${MODPATH}/webroot" 0 0 0755 0644
	fi

	flux_step_ok "Ownership and modes applied"
	return 0
}

# Read back what was applied. The runtime binaries are fatal because the daemon cannot start
# without the execute bit; the lifecycle scripts are fatal for the same reason the manager needs
# them; the WebUI is not, because a wrong mode there degrades the UI rather than the module.
flux_verify_permissions() {
	for _bin in fluxd flux_utility; do
		_path="${MODPATH}/system/bin/${_bin}"
		[ -f "${_path}" ] ||
			flux_abort "Missing runtime binary after installation: ${_bin}" \
				"Expected: ${_path}"
		flux_is_executable "${_path}" ||
			flux_abort "${_bin} is not executable after installation" \
				"Path: ${_path}" \
				"The filesystem did not accept the permission change." \
				"Flux cannot start without it."
	done
	flux_step_ok "Runtime binaries verified executable"

	for _script in service.sh uninstall.sh; do
		_path="${MODPATH}/${_script}"
		flux_is_executable "${_path}" ||
			flux_abort "${_script} is not executable after installation" \
				"Path: ${_path}" \
				"The module manager could not run it."
	done
	flux_step_ok "Lifecycle scripts verified executable"
	return 0
}
