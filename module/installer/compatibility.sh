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
# Platform compatibility gates and SoC family identification.
#
# The gates here are refusals, not warnings: an unsupported ABI or API level cannot be worked
# around at runtime, so installing anyway would produce a module that is present, enabled, and
# silently non-functional — the worst of the available outcomes, because the user has no signal
# that anything is wrong.
#
# SoC identification writes a family code that the runtime reads. It does NOT apply anything and
# does not mean any vendor capability is available: on-device capability certification happens in
# the execution engine, against the actual node set, long after this script is gone. The wording
# here is therefore strictly "detected", never "implementing" or "enabling" — an installer that
# announces vendor tuning it has not performed, and cannot verify, is lying to the user.

FLUX_MIN_API=28 # Android 9 (Pie)

# shellcheck disable=SC2034  # both are read by finalize.sh, which sources this file
FLUX_SOC=0
FLUX_SOC_NAME="unknown"

flux_check_android_version() {
	if [ "${API}" -lt "${FLUX_MIN_API}" ]; then
		flux_abort "Android ${API} is not supported." \
			"Flux requires Android 9 (Pie, API ${FLUX_MIN_API}) or newer."
	fi
	return 0
}

# Maps the manager-reported ARCH onto the ABI directory the package actually ships. An ABI with
# no payload aborts rather than falling back to another one: a 32-bit binary on a 64-bit-only
# device does not run, and picking "the closest available" would install something that cannot
# execute.
# shellcheck disable=SC2034  # FLUX_ABI_DIR is consumed by payload.sh and the environment report
flux_resolve_abi() {
	case "${ARCH}" in
	arm64) FLUX_ABI_DIR="arm64-v8a" ;;
	arm) FLUX_ABI_DIR="armeabi-v7a" ;;
	*)
		flux_abort "Unsupported CPU architecture: ${ARCH}" \
			"Flux ships arm64-v8a and armeabi-v7a runtimes." \
			"This device reports an architecture Flux has no runtime for."
		;;
	esac
	return 0
}

# ── SoC family identification ────────────────────────────────────────────────
# Family codes consumed by the runtime (written to ${FLUX_CONFIG_DIR}/soc_recognition):
#   1 MediaTek  2 Qualcomm Snapdragon  3 Exynos  4 Unisoc
#   5 Google Tensor  6 Nvidia Tegra    7 Kirin   0 unidentified

_flux_soc_from_string() {
	case "$1" in
	*mt* | *MT*) FLUX_SOC=1 FLUX_SOC_NAME="MediaTek" ;;
	*sm* | *qcom* | *SM* | *QCOM* | *Qualcomm*) FLUX_SOC=2 FLUX_SOC_NAME="Snapdragon" ;;
	*exynos* | *Exynos* | *EXYNOS* | *universal* | *samsung* | *erd* | *s5e*)
		FLUX_SOC=3 FLUX_SOC_NAME="Exynos"
		;;
	*Unisoc* | *unisoc* | *ums* | *UNISOC* | *sp* | *SC*) FLUX_SOC=4 FLUX_SOC_NAME="Unisoc" ;;
	*gs* | *Tensor* | *tensor*) FLUX_SOC=5 FLUX_SOC_NAME="Google Tensor" ;;
	*kirin*) FLUX_SOC=7 FLUX_SOC_NAME="Kirin" ;;
	esac
	[ "${FLUX_SOC}" -ne 0 ]
}

# Node-presence identification, tried first because a directory that exists is stronger evidence
# than a string that looks like a vendor name.
_flux_soc_from_nodes() {
	if [ -d /sys/class/kgsl/kgsl-3d0/devfreq ] || [ -d /sys/devices/platform/kgsl-2d0.0/kgsl ]; then
		FLUX_SOC=2 FLUX_SOC_NAME="Snapdragon"
		return 0
	fi
	if [ -d /sys/kernel/ged/hal ]; then
		FLUX_SOC=1 FLUX_SOC_NAME="MediaTek"
		return 0
	fi
	if [ -d /sys/kernel/tegra_gpu ]; then
		FLUX_SOC=6 FLUX_SOC_NAME="Nvidia Tegra"
		return 0
	fi
	return 1
}

_flux_soc_getprop() {
	for _prop in \
		ro.board.platform ro.soc.model ro.hardware ro.chipname \
		ro.hardware.chipname ro.vendor.soc.model.external_name \
		ro.vendor.qti.soc_name ro.vendor.soc.model.part_name ro.vendor.soc.model; do
		getprop "${_prop}" 2>/dev/null
	done
}

# shellcheck disable=SC2034  # FLUX_SOC_NAME is read by finalize.sh, which sources this file
flux_identify_soc() {
	FLUX_SOC=0
	FLUX_SOC_NAME="unknown"

	_flux_soc_from_nodes && return 0

	# `cat`, not `$(<file)`. The redirect-substitution form is a bash extension that busybox ash
	# does not implement, and Magisk installs run under busybox ash: it expanded to nothing, so
	# this identification tier silently never contributed a result.
	# /proc/device-tree entries are NUL-terminated, hence the tr.
	if [ -r /proc/device-tree/model ]; then
		_flux_soc_from_string "$(tr -d '\000' </proc/device-tree/model 2>/dev/null)" && return 0
	fi

	_flux_soc_from_string "$(_flux_soc_getprop)" && return 0

	_flux_soc_from_string "$(grep -E "Hardware|Processor" /proc/cpuinfo 2>/dev/null |
		uniq | cut -d ':' -f 2 | sed 's/^[ \t]*//')" && return 0

	_flux_soc_from_string "$(grep "model.name" /proc/cpuinfo 2>/dev/null |
		uniq | cut -d ':' -f 2 | sed 's/^[ \t]*//')" && return 0

	return 1
}
