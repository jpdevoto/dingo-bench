#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

format=$1
version=$2
build=$3
dcmtk=$4
output=$5
staging=$(mktemp -d)
app="$staging/root/usr/libexec/dingobench"
install -d "$output" "$app"
trap 'rm -rf -- "$staging"' EXIT

cmake --install "$build" --prefix "$app"
install -m 755 "$dcmtk/sdk/bin/wlmscpfs" "$app/"
install -m 644 "$dcmtk"/dcmtk-*/COPYRIGHT "$app/share/licenses/DingoBench/DCMTK-COPYRIGHT.txt"
install -m 644 docs/licenses/README.md "$app/share/licenses/DingoBench/THIRD-PARTY.md"
install -d "$staging/root/usr/bin" "$staging/root/usr/share/applications" \
  "$staging/root/usr/share/icons/hicolor/256x256/apps"
cat > "$staging/root/usr/bin/dingobench" <<'EOF'
#!/bin/sh
exec /usr/libexec/dingobench/DingoBench "$@"
EOF
chmod 755 "$staging/root/usr/bin/dingobench"
install -m 644 assets/branding/app-icon-256.png \
  "$staging/root/usr/share/icons/hicolor/256x256/apps/dingobench.png"
cat > "$staging/root/usr/share/applications/dingobench.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=DingoBench
Comment=Troubleshoot DICOM connectivity and exchanges
Exec=dingobench
Icon=dingobench
Categories=Utility;
Terminal=false
EOF

cat > "$staging/CPackConfig.cmake" <<EOF
set(CPACK_PACKAGE_NAME dingobench)
set(CPACK_PACKAGE_VERSION "$version")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Desktop DICOM troubleshooting tool")
set(CPACK_PACKAGE_CONTACT "Juan Pablo Devoto")
set(CPACK_PACKAGE_FILE_NAME "DingoBench-$version-linux-x86_64")
set(CPACK_INSTALLED_DIRECTORIES "$staging/root" "/")
set(CPACK_PACKAGING_INSTALL_PREFIX "/")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6widgets6, libqt6network6, libqt6svg6, qt6-qpa-plugins")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-only")
set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtbase-gui, qt6-qtsvg")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION "/usr/libexec" "/usr/share/applications"
  "/usr/share/icons" "/usr/share/icons/hicolor" "/usr/share/icons/hicolor/256x256"
  "/usr/share/icons/hicolor/256x256/apps")
EOF
cpack --config "$staging/CPackConfig.cmake" -G "$format" -B "$staging/packages"
case "$format" in
  DEB) extension=deb ;;
  RPM) extension=rpm ;;
  *) exit 2 ;;
esac
install -m 644 "$staging/packages/DingoBench-$version-linux-x86_64.$extension" "$output/"
