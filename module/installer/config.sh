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
# INTENTIONALLY EMPTY. Flux has no verified donation URL, and this must not be filled in with a
# guess. The consequences of getting it wrong are asymmetric: a wrong URL on a donation button
# sends a user's money, or their trust, somewhere the project does not control, and the user has
# no way to tell a mistake from an endorsement.
#
# Specifically rejected as a source: the `https://t.me/c/3901105851/3` link in
# webui/src/views/Home.vue. A t.me/c/<internal-id>/ link addresses a *private* channel by its
# internal id — it resolves only for accounts that are already members of that channel and fails
# for every other user, so it cannot serve as a public donation destination.
#
# While this is empty, every donate path stays inert and no donate button is shown or claimed:
#   - compile_zip.sh writes no `donate=` / `donateIcon=` key into module.prop
#   - action.sh reports that no donation destination is configured, and exits successfully
#   - the WebUI hides its support entry
#
# To enable, set this to the official https:// URL and rebuild. Nothing else needs to change.
OFFICIAL_DONATION_URL=""
