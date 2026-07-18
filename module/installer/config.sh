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
# Project-controlled constants. This is the ONLY place a user-facing destination is defined.
#
# Everything that opens a URL — action.sh, the packaging step that writes module.prop's donate
# metadata, the WebUI's support entry — reads from here. That is the point: there is exactly one
# place to audit, and no code path anywhere accepts a URL from input, from a file on the device,
# or from the network.
#
# Sourced by the installer helpers and by action.sh at runtime, and parsed (with sed, not
# sourced) by .github/scripts/compile_zip.sh at build time.

# shellcheck disable=SC2034  # every assignment here is consumed by whatever sources this file

# The repository this module is developed and released from.
FLUX_REPO_URL="https://github.com/FluxProjectsDev/Flux"
FLUX_ISSUES_URL="https://github.com/FluxProjectsDev/Flux/issues"

# Official donation destination.
#
# Supplied by the project maintainer (FebriCahyaa). SociaBuzz is a creator-support platform; the
# page is the maintainer's own. That provenance is the whole test applied here — a donation URL
# is not something to infer, because the consequences of getting it wrong are asymmetric: a wrong
# address sends a user's money, or their trust, somewhere the project does not control, and the
# user has no way to tell a mistake from an endorsement.
#
# Rejected as a source, and worth recording so nobody reaches for it again: the
# `https://t.me/c/3901105851/3` link that webui/src/views/Home.vue used to carry. A
# t.me/c/<internal-id>/ link addresses a *private* channel by its internal id — it resolves only
# for accounts already in that channel and fails for everyone else, so it could never have served
# as a public donation destination.
#
# Setting this enables every donate path at once, and nothing else needs to change:
#   - compile_zip.sh appends `donate=` and `donateIcon=` to module.prop
#   - action.sh opens it from the Action button on KernelSU and APatch
#   - the WebUI's support entry points here
#
# Clearing it back to "" disables all three again, and the fixtures cover both states.
#
# webui/src/views/Home.vue must carry the same URL — a WebUI page cannot source a shell file.
# verify-installer.sh asserts the two agree, so they cannot drift apart silently.
OFFICIAL_DONATION_URL="https://sociabuzz.com/fbrichy"
