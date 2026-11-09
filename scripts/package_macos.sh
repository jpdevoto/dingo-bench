#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

version=$1
build=$2
dcmtk=$3
qt=$4
output=$5
staging=$(mktemp -d)
trap 'rm -rf -- "$staging"' EXIT

app="$staging/image/DingoBench.app"
macos="$app/Contents/MacOS"
resources="$app/Contents/Resources"
install -d "$macos" "$resources/Notices" "$output"
install -m 755 "$build/DingoBench" "$build/DingoBenchReceiver" "$dcmtk/sdk/bin/wlmscpfs" "$macos/"
for executable in DingoBench DingoBenchReceiver wlmscpfs; do
  test "$(lipo -archs "$macos/$executable")" = arm64
done
install -m 644 LICENSE BRANDING.md "$resources/Notices/"
install -m 644 docs/licenses/README.md "$resources/Notices/THIRD-PARTY.md"
install -m 644 assets/material/LICENSE.txt "$resources/Notices/MaterialSymbols-LICENSE.txt"
install -m 644 assets/material/NOTICE.txt "$resources/Notices/MaterialSymbols-NOTICE.txt"
install -m 644 "$dcmtk"/dcmtk-*/COPYRIGHT "$resources/Notices/DCMTK-COPYRIGHT.txt"

python3 - "$app" "$version" <<'PY'
from pathlib import Path
import plistlib
import sys

app = Path(sys.argv[1])
version = sys.argv[2]
with (app / "Contents/Info.plist").open("wb") as plist:
    plistlib.dump({
        "CFBundleIdentifier": "io.github.jpdevoto.dingobench",
        "CFBundleName": "DingoBench",
        "CFBundleDisplayName": "DingoBench",
        "CFBundleExecutable": "DingoBench",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": version,
        "NSHighResolutionCapable": True,
    }, plist)
PY

PYTHONPATH=scripts python3 - "$resources/Notices/Qt" "$dcmtk" <<'PY'
from pathlib import Path
import sys
from dependency_sources import QTBASE, QTSVG, fetch
from package_windows import qt_notices

destination, cache = map(Path, sys.argv[1:])
for source in (QTBASE, QTSVG):
    qt_notices(fetch(source, cache / source.name), destination)
PY

"$qt/bin/macdeployqt" "$app" "-executable=$macos/DingoBenchReceiver" "-executable=$macos/wlmscpfs" -codesign=-
test -d "$app/Contents/Frameworks/QtCore.framework"
test -f "$app/Contents/PlugIns/platforms/libqcocoa.dylib"
codesign --verify --deep --strict "$app"
codesign -dv --verbose=4 "$app" 2>&1 | grep -Fx 'Signature=adhoc'
ln -s /Applications "$staging/image/Applications"
dmg="$output/DingoBench-$version-macos-arm64.dmg"
hdiutil create -volname DingoBench -srcfolder "$staging/image" -format UDZO "$dmg"
hdiutil verify "$dmg"
