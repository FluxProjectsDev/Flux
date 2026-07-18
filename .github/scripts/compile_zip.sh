#!/bin/env bash
# shellcheck disable=SC2035

if [ -z "$GITHUB_WORKSPACE" ]; then
	echo "This script should only run on GitHub action!" >&2
	exit 1
fi

# Make sure we're on right directory
cd "$GITHUB_WORKSPACE" || {
	echo "Unable to cd to GITHUB_WORKSPACE" >&2
	exit 1
}

# Version info
version="$(cat version)"
version_code="$(git rev-list HEAD --count)"
release_code="$(git rev-list HEAD --count)-$(git rev-parse --short HEAD)-release"
sed -i "s/version=.*/version=$version ($release_code)/" module/module.prop
sed -i "s/versionCode=.*/versionCode=$version_code/" module/module.prop

# Point the module manager (Magisk / KernelSU / APatch) at this repository's update manifest.
# It polls this URL, compares versionCode against the installed module, and offers a direct
# download + install. `releases/latest` always resolves to the newest published release, so the
# field never needs updating. Derived from GITHUB_REPOSITORY so a fork advertises its own
# releases instead of someone else's.
sed -i "s|updateJson=.*|updateJson=https://github.com/$GITHUB_REPOSITORY/releases/latest/download/update.json|" module/module.prop

# Copy module files
cp -r ./libs module

# Only the device runtime scripts ship. scripts/ also holds CI/development tooling
# (fetch-synthesiscore, verify-synthesiscore, update-synthesiscore-lock) which has no meaning on
# a device: customize.sh never installs it, so copying everything only padded the zip with dead
# tooling. This is an allowlist on purpose — a new script has to be named here to ship, rather
# than reaching users because it happened to land in the directory.
# flux_utility is the only runtime script left. flux_profiler.sh was the legacy shell applier
# and is gone: the V2 ExecutionEngine applies profiles, and packaging a script nothing can invoke
# would leave a working copy of the old write path on every device for someone to run by hand.
# This was a loop over both; a loop over one entry is just a copy.
cp ./scripts/flux_utility.sh module/system/bin/ || {
	echo "Missing runtime script: scripts/flux_utility.sh" >&2
	exit 1
}
cp gamelist.txt module
cp LICENSE module
cp NOTICE.md module
cp banner.webp module

find ./prebuilt -mindepth 1 -maxdepth 1 ! -name "README.md" -exec cp -r {} ./module \;
find ./config -mindepth 1 -maxdepth 1 ! -name "README.md" -exec cp -r {} ./module/config \;

# Remove .sh extension from scripts
find module/system/bin -maxdepth 1 -type f -name "*.sh" -exec sh -c 'mv -- "$0" "${0%.sh}"' {} \;

# Parse version info to module prop
zipName="flux-$version-$release_code.zip"
echo "zipName=$zipName" >>"$GITHUB_OUTPUT"

# Generate sha256sum for integrity checkup
bash .github/scripts/gen_sha256sum.sh "module"

cd ./module || {
	echo "Unable to cd to ./module" >&2
	exit 1
}

# Zip the file
zip -r9 ../"$zipName" * -x *placeholder* *.map .shellcheckrc
zip -z ../"$zipName" <<EOF
$version-$release_code
Build Date $(date +"%a %b %d %H:%M:%S %Z %Y")
EOF
