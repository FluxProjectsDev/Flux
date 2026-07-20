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
# Flux installer entry point.
#
# This file is deliberately thin. It is the portable contract every module manager understands —
# it sequences the stages and does nothing else. All logic lives in installer/, where it can be
# read, checksummed and tested on its own.
#
# The stage numbering is honest: [n/8] advances when a stage starts, so it can never run ahead of
# the work, and every [OK] in this run follows a check that actually executed. There are no
# sleeps, no spinners and no progress animation anywhere in the installer — a flash is fast, and
# padding it to look busy would be the one thing here that is purely a lie.
#
# Failure handling is structural rather than conventional: flux_abort does not return, so the
# summary at the end is unreachable after a fatal error. "Success cannot print after a failure"
# is not a rule someone has to remember to follow.

# shellcheck disable=SC1091  # helpers are extracted from the ZIP at run time
# shellcheck disable=SC2034  # SKIPUNZIP is read by the module manager, not by this script
SKIPUNZIP=1

# ── Bootstrap ────────────────────────────────────────────────────────────────
# verify.sh is the trust root: it verifies every installer helper against its packaged digest
# before sourcing it. See that file for what this does and does not guarantee.
ui_print "- Loading Flux installer"
unzip -o "$ZIPFILE" 'verify.sh' -d "$TMPDIR" >/dev/null 2>&1
if [ ! -s "$TMPDIR/verify.sh" ]; then
	ui_print "*********************************************************"
	ui_print "! Unable to extract verify.sh"
	ui_print "! The package is corrupted. Please re-download Flux."
	abort "*********************************************************"
fi
. "$TMPDIR/verify.sh"
flux_bootstrap_installer

flux_ui_init
flux_print_banner

# ── [1/8] Package integrity ──────────────────────────────────────────────────
flux_step_begin "Package integrity"
flux_info "Verifying package..."
flux_integrity_init || flux_abort "Cannot create the verification work directory" \
	"TMPDIR is not writable."
flux_step_ok "Package integrity verified"
flux_info "Installer components checksum-verified"
_ub_state="$(flux_check_update_binary)"
case "${_ub_state}" in
verified) flux_step_ok "Installer stub checksum verified" ;;
unsigned) flux_info "Installer stub has no checksum (installed via the manager's own updater)" ;;
*) flux_info "No installer stub in this package" ;;
esac

# ── [2/8] Installation environment ───────────────────────────────────────────
flux_step_begin "Installation environment"
flux_detect_environment
flux_resolve_abi
flux_report_environment
if [ "${FLUX_MANAGER}" = "unknown" ]; then
	# Reached only when the module contract check in flux_detect_environment passed, so the
	# variables and functions the installer needs are all present. Proceeding is safe; guessing
	# manager-private paths would not be, and nothing below does.
	flux_step_warn "Unrecognised module manager; continuing on the standard module contract"
else
	flux_step_ok "Module manager recognised"
fi

# ── [3/8] Architecture and Android compatibility ─────────────────────────────
flux_step_begin "Architecture and Android compatibility"
flux_check_android_version
flux_step_ok "Android API ${FLUX_API} meets the minimum (${FLUX_MIN_API})"
flux_step_ok "Architecture ${FLUX_ABI_DIR} supported"

# ── [4/8] Existing installation and configuration ────────────────────────────
flux_step_begin "Existing installation and configuration"
flux_migrate_existing

# ── [5/8] Runtime and SynthesisCore payload ──────────────────────────────────
flux_step_begin "Runtime and SynthesisCore payload"
flux_install_module_files
flux_install_runtime
flux_stage_config

# ── [6/8] WebUI and module metadata ──────────────────────────────────────────
flux_step_begin "WebUI and module metadata"
flux_install_webui
flux_configure_manager_integration

# ── [7/8] Permissions and configuration ──────────────────────────────────────
# Permissions are applied before the configuration settles because flux_settle_config runs the
# daemon to adjudicate the existing gamelist, and the daemon needs its execute bit first.
flux_step_begin "Permissions and configuration"
flux_apply_permissions
flux_verify_permissions
flux_settle_config

# ── [8/8] Final verification ─────────────────────────────────────────────────
flux_step_begin "Final verification"
flux_finalize

# ── Result ───────────────────────────────────────────────────────────────────
# Reachable only because no stage aborted.
if [ "${FLUX_WARN_COUNT}" -gt 0 ]; then
	flux_summary LIMITED
else
	flux_summary SUCCESS
fi
