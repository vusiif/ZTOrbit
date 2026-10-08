#!/bin/bash
set -euo pipefail

if [[ "$(uname -s)" != Darwin ]]; then
    echo "This script requires macOS, Xcode Command Line Tools, and a Qt SDK with arm64 libraries." >&2
    exit 1
fi

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_directory="$project_root/cmake-build-macos-arm64"
output_directory="$project_root/dist/macos-arm64"

if [[ -z "${QT_ROOT:-}" ]] && command -v brew >/dev/null 2>&1; then
    QT_ROOT="$(brew --prefix qt)"
fi
if [[ -z "${QT_ROOT:-}" ]] || [[ ! -x "$QT_ROOT/bin/macdeployqt" ]]; then
    echo "Set QT_ROOT to a Qt 6.8 or newer macOS SDK directory (containing bin/macdeployqt)." >&2
    exit 1
fi
command -v cmake >/dev/null
xcrun --find clang++ >/dev/null
cmake -S "$project_root" -B "$build_directory" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_ROOT" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DZEROTIER_BUILD_TESTS=ON
cmake --build "$build_directory" --parallel "$(sysctl -n hw.logicalcpu)"
ctest --test-dir "$build_directory" --output-on-failure

staging="$(mktemp -d "$build_directory/package.XXXXXX")"
trap '/bin/rm -rf -- "$staging"' EXIT
/usr/bin/ditto "$build_directory/ZTGUI.app" "$staging/ZTGUI.app"

iconset="$staging/app-icon.iconset"
/bin/mkdir -p "$iconset" "$staging/ZTGUI.app/Contents/Resources"
for size in 16 32 128 256 512; do
    /usr/bin/sips -z "$size" "$size" "$project_root/assets/app-icon.png" \
        --out "$iconset/icon_${size}x${size}.png" >/dev/null
    retina=$((size * 2))
    /usr/bin/sips -z "$retina" "$retina" "$project_root/assets/app-icon.png" \
        --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
/usr/bin/iconutil -c icns "$iconset" -o "$staging/ZTGUI.app/Contents/Resources/app-icon.icns"
plist="$staging/ZTGUI.app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Set :CFBundleIconFile app-icon.icns' "$plist" || \
    /usr/libexec/PlistBuddy -c 'Add :CFBundleIconFile string app-icon.icns' "$plist"
(
    cd "$staging"
    "$QT_ROOT/bin/macdeployqt" ZTGUI.app -always-overwrite -codesign=- -dmg
)
/usr/bin/lipo -verify_arch arm64 "$staging/ZTGUI.app/Contents/MacOS/ZTGUI"
/usr/bin/codesign --verify --deep --strict "$staging/ZTGUI.app"
/bin/mkdir -p "$output_directory"
/bin/cp "$staging/ZTGUI.dmg" "$output_directory/ZTGUI-arm64.dmg"
# Preserve the bundle in a ZIP as well; ditto keeps executable permissions and symlinks.
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$staging/ZTGUI.app" "$output_directory/ZTGUI-arm64.app.zip"
echo "Created $output_directory/ZTGUI-arm64.dmg"
echo "Created $output_directory/ZTGUI-arm64.app.zip"
