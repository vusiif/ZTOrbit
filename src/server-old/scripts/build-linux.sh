#!/bin/bash
# Archived C++17/Qt6 ZTServer build entry. The supported server is the Java
# implementation under src/server; this script exists only for the old project.
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd -P)"
voice=OFF
for argument in "$@"; do
    case "$argument" in
        --build-voice-plugin) voice=ON ;;
        *) echo "Usage: $0 [--build-voice-plugin]" >&2; exit 2 ;;
    esac
done
[[ "$(uname -s)" == Linux ]] || { echo 'Linux required' >&2; exit 1; }
cd "$project_root"
build="$project_root/build/cmake-build-server-old"
if [[ -f "$build/CMakeCache.txt" ]] && ! grep -Fxq "CMAKE_HOME_DIRECTORY:INTERNAL=$project_root/src/server-old" "$build/CMakeCache.txt"; then
    rm -rf -- "$build"
fi
cmake -S src/server-old -B "$build" -DCMAKE_BUILD_TYPE=Release -DZTSERVER_BUILD_VOICE_PLUGIN="$voice" -DZTSERVER_BUILD_TESTS=OFF
cmake --build "$build" --target zt-server --parallel
mkdir -p build/dist
stage="$(mktemp -d "$project_root/build/dist/.linux-x64-cpp.XXXXXX")"
cp "$build/zt-server" src/server-old/main/zt-server.service src/server-old/VERSION "$stage/"
cp src/server-old/main/start-server.sh "$stage/"
chmod 755 "$stage/zt-server" "$stage/start-server.sh"
cp src/server-old/DEPLOYMENT.md "$stage/README.md"
if [[ -d build/dist/linux-x64-cpp ]]; then rm -rf -- "build/dist/linux-x64-cpp"; fi
mv "$stage" build/dist/linux-x64-cpp
tar -czf build/dist/linux-x64-cpp.tar.gz -C build/dist/linux-x64-cpp .

python3 src/server-old/scripts/package-server-sdk.py "$project_root"
if [[ "$voice" == ON ]]; then
    cmake --build "$build" --target ztserver_voice_chat_package --parallel
    mkdir -p build/dist/server-plugins-cpp
    cp "$build/plugins/ztserver_voice_chat.ztplugin" build/dist/server-plugins-cpp/ztserver_voice_chat-v0.1.0.ztplugin
fi
echo 'Created build/dist/linux-x64-cpp.tar.gz (archived C++ server)'
