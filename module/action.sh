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
# The module manager's Action button.
#
# What this script does depends on what the manager can already do for itself, because the Action
# button is only worth spending on whatever the user cannot otherwise reach:
#
#   KernelSU / APatch  Their module cards already have a WebUI button. Action is therefore spent
#                      on Support/Donate, which has no other entry point.
#   Magisk             Has no WebUI button at all. Action is spent on opening the WebUI, which is
#                      the higher-value action and the historical behaviour of this file.
#   MMRL               Opens the WebUI from the card itself and does not want a script doing it.
#
# Note what changed and why it mattered: customize.sh used to DELETE this file whenever KernelSU
# or APatch was detected. That is exactly backwards. Magisk has no Action button, so the copy that
# survived was on the one manager that could never invoke it, while the two managers whose only
# action mechanism this is had it removed. The deletion is gone; nothing here replaces it.
#
# Security properties, all of them structural rather than validated:
#   - Every URL is a compile-time constant from installer/config.sh. Nothing is read from input,
#     from a file on the device, from a property, or from the network.
#   - No eval, no constructed command, no shell interpolation of anything user-supplied.
#   - This script performs no network request of its own. It hands a URL to Android and stops.
#   - It never opens a donation page on its own; only a deliberate tap on Action reaches here.

MODDIR="${0%/*}"

# Constants live in one auditable place. If it is missing the script degrades to printing
# information rather than guessing a destination.
if [ -f "${MODDIR}/installer/config.sh" ]; then
	# shellcheck source=module/installer/config.sh
	. "${MODDIR}/installer/config.sh"
fi
FLUX_REPO_URL="${FLUX_REPO_URL:-https://github.com/FluxProjectsDev/Flux}"
OFFICIAL_DONATION_URL="${OFFICIAL_DONATION_URL:-}"

# open_url <url>
# Launches an explicit Android VIEW intent. Fails gracefully rather than silently: if the
# Activity Manager is unavailable (some recovery and headless contexts), the URL is printed so
# the user can still act on it.
open_url() {
	if ! command -v am >/dev/null 2>&1; then
		echo "- Cannot open a browser from here (Activity Manager unavailable)."
		echo "- Open this address manually:"
		echo "    $1"
		return 1
	fi
	if am start -a android.intent.action.VIEW -d "$1" >/dev/null 2>&1; then
		return 0
	fi
	echo "- Could not hand the link to an app."
	echo "- Open this address manually:"
	echo "    $1"
	return 1
}

show_support_info() {
	echo "- Flux Support"
	echo ""
	if [ -n "${OFFICIAL_DONATION_URL}" ]; then
		echo "- Opening the official support page..."
		open_url "${OFFICIAL_DONATION_URL}"
		return 0
	fi
	# Deliberately not a fallback to some other link. Flux has no verified donation destination,
	# and sending a user who tapped "Support" somewhere they did not ask to go — or to a link that
	# fails for them — is worse than telling them plainly that there is nothing to open yet.
	echo "- Flux does not currently have an official donation page."
	echo "- Nothing is being collected, and no link will be opened."
	echo ""
	echo "- The best support is a good bug report:"
	echo "    ${FLUX_REPO_URL}"
	return 0
}

launch_webui() {
	# WebUI X, then its older package name, then the KernelSU standalone viewer. Each is checked
	# for presence before being addressed, so a missing app produces the next option rather than a
	# failed intent.
	if pm path com.dergoogler.mmrl.wx >/dev/null 2>&1; then
		echo "- Opening Flux in WebUI X..."
		am start -n "com.dergoogler.mmrl.wx/.ui.activity.webui.WebUIActivity" -e MOD_ID "flux" \
			>/dev/null 2>&1 && return 0
	fi
	if pm path com.dergoogler.mmrl.webuix >/dev/null 2>&1; then
		echo "- Opening Flux in WebUI X..."
		am start -n "com.dergoogler.mmrl.webuix/.ui.activity.webui.WebUIActivity" -e MOD_ID "flux" \
			>/dev/null 2>&1 && return 0
	fi
	if pm path io.github.a13e300.ksuwebui >/dev/null 2>&1; then
		echo "- Opening Flux in KSUWebUIStandalone..."
		am start -n "io.github.a13e300.ksuwebui/.WebUIActivity" -e id "flux" \
			>/dev/null 2>&1 && return 0
	fi

	echo "- No WebUI viewer is installed."
	echo "- Flux's interface needs one of: WebUI X, or KSUWebUIStandalone."
	echo "- Opening the WebUI X release page..."
	open_url "https://github.com/MMRLApp/WebUI-X-Portable/releases"
	return 0
}

# ── Dispatch ─────────────────────────────────────────────────────────────────
if [ -n "${MMRL:-}" ]; then
	echo "- MMRL opens the Flux WebUI from the module card itself."
	echo "- Tap the card rather than this action."
	exit 0
fi

# APatch before KernelSU: APatch also sets KSU=true, so the KSU test alone cannot tell them apart.
if [ "${APATCH:-}" = "true" ] || [ -n "${APATCH_VER_CODE:-}" ]; then
	show_support_info
	exit 0
fi
if [ "${KSU:-}" = "true" ] || [ -n "${KSU_VER_CODE:-}" ]; then
	show_support_info
	exit 0
fi

# Magisk, or anything else that got this far: the WebUI is what it cannot otherwise reach.
launch_webui
exit 0
