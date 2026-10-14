#!/bin/bash
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd -P)"
voice=OFF
for argument in "$@"; do
    case "$argument" in
        --build-voice-plugin) voice=ON ;;
        *) echo "Usage: $0 [--build-voice-plugin]" >&2; exit 2 ;;
    esac
done
[[ "$(uname -s)" == Linux ]] || { echo 'Linux required' >&2; exit 1; }
cd "$project_root"
build="$project_root/cmake-build-server"
if [[ -f "$build/CMakeCache.txt" ]] && ! grep -Fxq "CMAKE_HOME_DIRECTORY:INTERNAL=$project_root/src/server" "$build/CMakeCache.txt"; then
    mv "$build" "$build.previous-$(date +%s)-$RANDOM"
fi
cmake -S src/server -B "$build" -DCMAKE_BUILD_TYPE=Release -DZTSERVER_BUILD_VOICE_PLUGIN="$voice" -DZTSERVER_BUILD_TESTS=OFF
cmake --build "$build" --target zt-server --parallel
mkdir -p dist
stage="$(mktemp -d "$project_root/dist/.linux-x64.XXXXXX")"
cp "$build/zt-server" src/server/main/zt-server.service VERSION "$stage/"
cp src/server/DEPLOYMENT.md "$stage/README.md"
if [[ -d dist/linux-x64 ]]; then mv dist/linux-x64 "dist/linux-x64.previous-$(date +%s)-$RANDOM"; fi
mv "$stage" dist/linux-x64
tar -czf dist/linux-x64.tar.gz -C dist/linux-x64 .

# The SDK fingerprint also includes the shared public headers.
python3 .agent/skills/ztgui-build/scripts/package-server-sdk.py "$project_root"
if [[ "$voice" == ON ]]; then
    cmake --build "$build" --target ztserver_voice_chat_package --parallel
    mkdir -p dist/server-plugins
    cp "$build/plugins/ztserver_voice_chat.ztplugin" dist/server-plugins/
fi
echo 'Created dist/linux-x64.tar.gz (plugins published separately)'
