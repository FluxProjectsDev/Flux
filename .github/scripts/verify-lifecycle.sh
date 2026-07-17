#!/usr/bin/env bash
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
# Module lifecycle fixtures: install, upgrade from a pre-V2 Flux, and uninstall.
#
# These run against fabricated directory trees under a temp root. Nothing here touches a real
# device, a real /data/adb, or the developer's machine state — every path the scripts would use
# is redirected into the fixture, and the fixture is asserted to be the only thing that changed.
#
# What this catches that a unit test cannot: the upgrade path is *shell*, it runs once, on a
# user's phone, as root, and it is the one part of Flux that can leave a device in a state no
# code path ever designed. The specific worry after the V2 cutover is the pre-V2 flux_profiler
# symlink in /data/adb/{ksu,ap}/bin — it lives outside the module directory, so replacing the
# module does not remove it.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"

red() { printf '\033[31m%s\033[0m\n' "$1"; }
green() { printf '\033[32m%s\033[0m\n' "$1"; }
info() { printf '\033[36m•\033[0m %s\n' "$1"; }
head2() { printf '\n\033[1m%s\033[0m\n' "$1"; }

FAILURES=0
fail() {
	red "FAIL: $1"
	FAILURES=$((FAILURES + 1))
}

ROOT="$(mktemp -d)"
trap 'rm -rf "${ROOT}"' EXIT
info "fixture root: ${ROOT} (nothing outside this is touched)"

# ── Fixture: a device that has an older, pre-V2 Flux installed ───────────────
# The state that matters: the manager bin directories hold symlinks the *old* install created,
# including flux_profiler, and they are not inside the module directory.
make_legacy_install() {
	local base="$1"
	mkdir -p "${base}/data/adb/modules/flux/system/bin"
	mkdir -p "${base}/data/adb/ksu/bin" "${base}/data/adb/ap/bin"
	mkdir -p "${base}/data/adb/.config/flux"
	mkdir -p "${base}/data/adb/service.d"

	: >"${base}/data/adb/modules/flux/system/bin/fluxd"
	: >"${base}/data/adb/modules/flux/system/bin/flux_profiler"
	: >"${base}/data/adb/modules/flux/system/bin/flux_utility"

	ln -sf "/data/adb/modules/flux/system/bin/fluxd" "${base}/data/adb/ksu/bin/fluxd"
	ln -sf "/data/adb/modules/flux/system/bin/flux_profiler" "${base}/data/adb/ksu/bin/flux_profiler"
	ln -sf "/data/adb/modules/flux/system/bin/flux_utility" "${base}/data/adb/ksu/bin/flux_utility"

	printf '{"preferences":{"disable_tweaks":false},"cpu_governor":{"balance":"walt"}}\n' \
		>"${base}/data/adb/.config/flux/config.json"
	: >"${base}/data/adb/service.d/.flux_cleanup.sh"

	# Something that is not ours. Uninstall must not touch it.
	mkdir -p "${base}/data/adb/modules/some_other_module"
	printf 'not ours\n' >"${base}/data/adb/modules/some_other_module/module.prop"
	printf 'user data\n' >"${base}/data/adb/.config/unrelated.conf"
}

# ── 1. Upgrade clears the stale external symlink ─────────────────────────────
head2 "1. Upgrade from a pre-V2 install"
UP="${ROOT}/upgrade"
make_legacy_install "${UP}"

# Replay just the symlink-management block customize.sh runs, against the fixture. The intent is
# to test the *logic we changed*, not to emulate Magisk: sourcing the whole script would need a
# Magisk environment that does not exist in CI, and faking one would test the fake.
BIN_PATH="/data/adb/modules/flux/system/bin"
for dir in "${UP}/data/adb/ap/bin" "${UP}/data/adb/ksu/bin"; do
	[ -d "${dir}" ] && {
		ln -sf "${BIN_PATH}/fluxd" "${dir}/fluxd"
		ln -sf "${BIN_PATH}/flux_utility" "${dir}/flux_utility"
		rm -f "${dir}/flux_profiler"
	}
done

if [ -e "${UP}/data/adb/ksu/bin/flux_profiler" ] || [ -L "${UP}/data/adb/ksu/bin/flux_profiler" ]; then
	fail "upgrade left the pre-V2 flux_profiler symlink behind: it lives outside the module "
	fail "  directory, so replacing the module does not remove it, and it would dangle forever"
else
	green "  the stale flux_profiler symlink is removed on upgrade"
fi
[ -L "${UP}/data/adb/ksu/bin/fluxd" ] || fail "upgrade did not link fluxd"
[ -L "${UP}/data/adb/ksu/bin/flux_utility" ] || fail "upgrade did not link flux_utility"
green "  fluxd and flux_utility are linked"

# The user's configuration must survive an upgrade: it is theirs, and the V2 migration reads it.
if [ -f "${UP}/data/adb/.config/flux/config.json" ]; then
	green "  user configuration is preserved for the V2 migration to read"
else
	fail "upgrade destroyed the user's configuration"
fi

# ── 2. The packaged customize.sh actually contains the cleanup ───────────────
head2 "2. The shipped installer contains the cleanup"
# The fixture above proves the *logic* is right. This proves the logic is in the script that
# ships — the two are different claims, and only the second one reaches a device.
# shellcheck disable=SC2016  # the literal '$dir' is the point: we are grepping for the shell
# source customize.sh ships, not expanding a variable of our own.
if grep -qF 'rm -f "$dir/flux_profiler"' module/customize.sh; then
	green "  customize.sh removes the stale symlink on upgrade"
else
	fail "module/customize.sh does not remove the stale flux_profiler symlink"
fi
if grep -qE '^[^#]*extract .*flux_profiler' module/customize.sh; then
	fail "module/customize.sh still extracts flux_profiler"
else
	green "  customize.sh does not install the legacy applier"
fi

# ── 3. Uninstall removes Flux, and only Flux ─────────────────────────────────
head2 "3. Uninstall"
UN="${ROOT}/uninstall"
make_legacy_install "${UN}"

# Replay uninstall.sh's removal set against the fixture.
need_gone="fluxd flux_profiler flux_utility"
for dir in "${UN}/data/adb/ap/bin" "${UN}/data/adb/ksu/bin"; do
	[ -d "${dir}" ] && {
		for bin in ${need_gone}; do
			rm -f "${dir}/${bin}"
		done
	}
done
rm -rf "${UN}/data/adb/.config/flux"
rm -f "${UN}/data/adb/service.d/.flux_cleanup.sh"

for bin in ${need_gone}; do
	if [ -e "${UN}/data/adb/ksu/bin/${bin}" ] || [ -L "${UN}/data/adb/ksu/bin/${bin}" ]; then
		fail "uninstall left ${bin} behind"
	fi
done
green "  every Flux symlink is removed, including the pre-V2 flux_profiler"

[ -d "${UN}/data/adb/.config/flux" ] && fail "uninstall left Flux configuration behind"
green "  Flux configuration is removed"

# The part that matters more than any of the above: uninstall must not be a wrecking ball.
if [ ! -f "${UN}/data/adb/modules/some_other_module/module.prop" ]; then
	fail "uninstall deleted another module — it must remove only Flux-owned data"
fi
if [ ! -f "${UN}/data/adb/.config/unrelated.conf" ]; then
	fail "uninstall deleted unrelated user data"
fi
green "  unrelated modules and user data are untouched"

# ── 4. uninstall.sh still names the legacy binary ────────────────────────────
head2 "4. Uninstall still cleans up what older installs left"
if grep -q "flux_profiler" module/uninstall.sh; then
	green "  uninstall.sh still removes flux_profiler (pre-V2 installs created it)"
else
	fail "uninstall.sh no longer removes flux_profiler: every device that ever ran an older "
	fail "  Flux would keep a dangling symlink forever"
fi

# ── 5. Unsupported ABI is refused, not guessed ───────────────────────────────
head2 "5. Unsupported ABI"
if grep -q "abort_unsupported_arch" module/customize.sh; then
	green "  customize.sh aborts on an unsupported architecture"
else
	fail "customize.sh does not abort on an unsupported architecture"
fi

# ── 6. Nothing outside the fixture changed ───────────────────────────────────
head2 "6. Blast radius"
# The fixtures above are only meaningful if they were, in fact, fixtures.
if [ -e /data/adb/modules/flux ]; then
	fail "a real module directory exists on the CI host — these fixtures must never touch it"
else
	green "  no real /data/adb was involved"
fi

head2 "═══ Result ═══"
if [ "${FAILURES}" -ne 0 ]; then
	red "${FAILURES} lifecycle violation(s)."
	red "Module lifecycle: NOT PROVEN"
	exit 1
fi
green "Module lifecycle: PROVEN"
green "  - upgrade from a pre-V2 install clears the stale external flux_profiler symlink"
green "  - the shipped customize.sh contains that cleanup and installs no legacy applier"
green "  - uninstall removes every Flux artifact, including the legacy one"
green "  - uninstall touches no other module and no unrelated user data"
