#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds Livecut's macOS installer from a Release build's artefacts.
#
#   make-installer.sh <artefacts> <version> <output> --sign <application identity> <installer identity>
#   make-installer.sh <artefacts> <version> <output> --unsigned
#
# <artefacts> is build/Livecut_artefacts/Release. The identities are the full
# names of your "Developer ID Application" and "Developer ID Installer"
# certificates. --unsigned is for checking the installer's layout locally; an
# unsigned installer can't be notarised and Gatekeeper will refuse it.
#
# The installer puts the VST3 and AU in /Library/Audio/Plug-Ins and the
# standalone app in /Applications. Signing uses the hardened runtime and a
# secure timestamp, both of which notarisation requires.
set -euo pipefail

usage() {
    sed -n '4,16p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 64
}

[[ $# -ge 4 ]] || usage
artefacts=$1
version=$2
output=$3
mode=$4

case "$mode" in
    --sign)
        [[ $# -eq 6 ]] || usage
        application_identity=$5
        installer_identity=$6
        ;;
    --unsigned)
        [[ $# -eq 4 ]] || usage
        ;;
    *)
        usage
        ;;
esac

script_dir=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Packages one bundle so it installs at <location>, signing it first unless --unsigned.
package() {
    local bundle=$1 location=$2 name=$3 entitlements=${4:-}
    local root="$work/root-$name"
    local staged="$root/$(basename "$bundle")"

    # pkgbuild archives extended attributes and Installer restores them, so stage
    # a copy without them; a quarantine flag would make Gatekeeper block the plug-in.
    ditto --noextattr --noqtn "$bundle" "$staged"

    if [[ $mode == --sign ]]; then
        local options=(--force --timestamp --options runtime --sign "$application_identity")
        [[ -n $entitlements ]] && options+=(--entitlements "$entitlements")
        codesign "${options[@]}" "$staged"
        codesign --verify --strict --verbose=2 "$staged"
    fi

    # Plug-ins and the app must install where the installer says, never be
    # "relocated" to wherever an older copy happens to live.
    pkgbuild --analyze --root "$root" "$work/$name.plist"
    plutil -replace 0.BundleIsRelocatable -bool NO "$work/$name.plist"

    pkgbuild --root "$root" \
             --component-plist "$work/$name.plist" \
             --install-location "$location" \
             --identifier "com.seandearnaley.livecut.$name" \
             --version "$version" \
             "$work/packages/$name.pkg"
}

mkdir -p "$work/packages" "$output"
package "$artefacts/VST3/Livecut.vst3" "/Library/Audio/Plug-Ins/VST3" vst3
package "$artefacts/AU/Livecut.component" "/Library/Audio/Plug-Ins/Components" au
package "$artefacts/Standalone/Livecut.app" "/Applications" app "$script_dir/Standalone.entitlements"

productbuild --synthesize \
             --package "$work/packages/vst3.pkg" \
             --package "$work/packages/au.pkg" \
             --package "$work/packages/app.pkg" \
             "$work/distribution.xml"

installer="$output/Livecut-$version.pkg"
product_options=(--distribution "$work/distribution.xml" --package-path "$work/packages")
[[ $mode == --sign ]] && product_options+=(--sign "$installer_identity" --timestamp)
productbuild "${product_options[@]}" "$installer"

echo "Built $installer"
