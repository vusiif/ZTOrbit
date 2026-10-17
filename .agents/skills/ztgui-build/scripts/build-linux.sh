#!/bin/bash
# Build the Java ZTServer host package, the Java plugin SDK and optional Java plugins.
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

java_command=''
if [[ -n "${JAVA_HOME:-}" && -x "$JAVA_HOME/bin/java" ]]; then
    java_command="$JAVA_HOME/bin/java"
elif command -v java >/dev/null 2>&1; then
    java_command="$(command -v java)"
fi
[[ -n "$java_command" ]] || { echo 'JDK 25 is required (set JAVA_HOME or PATH)' >&2; exit 1; }
if [[ -n "${JAVA_HOME:-}" && -x "$JAVA_HOME/bin/java" ]]; then
    export PATH="$JAVA_HOME/bin:$PATH"
fi
java_major="$("$java_command" -version 2>&1 | sed -n 's/.*version "\([0-9][0-9]*\).*/\1/p' | head -1)"
[[ -n "$java_major" && "$java_major" -ge 25 ]] || {
    echo "JDK 25 or newer is required; found ${java_major:-unknown}" >&2; exit 1; }
mvn -version >/dev/null 2>&1 || { echo 'Maven 3.9+ is required' >&2; exit 1; }

revision="$(tr -d '\r\n' < SERVER_VERSION)"
[[ "$revision" =~ ^0\.([0-9]+)\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]] || {
    echo "SERVER_VERSION must be 0.<core>.<patch>[-qualifier]: $revision" >&2; exit 1; }
api_version="0.${BASH_REMATCH[1]}${BASH_REMATCH[2]}"
echo "Building ZTServer $revision (API $api_version)"
mvn -f src/server/pom.xml "-Drevision=$revision" "-DapiVersion=$api_version" clean verify

dist="$project_root/build/dist"
mkdir -p "$dist"
stage="$(mktemp -d "$dist/.linux-x64.XXXXXX")"
trap 'rm -rf -- "$stage"' EXIT
cp src/server/main/target/zt-server.jar "$stage/zt-server.jar"
cp src/server/scripts/start-server.sh src/server/scripts/zt-server.service "$stage/"
cp src/server/scripts/zt-installer.sh "$stage/"
cp src/server/DEPLOYMENT.md "$stage/README.md"
cp SERVER_VERSION "$stage/VERSION"
cp -R src/server/licenses "$stage/licenses"
chmod 755 "$stage" "$stage/start-server.sh" "$stage/zt-installer.sh"
chmod 644 "$stage/zt-server.jar" "$stage/zt-server.service" "$stage/README.md" "$stage/VERSION"
find "$stage/licenses" -type d -exec chmod 755 {} +
find "$stage/licenses" -type f -exec chmod 644 {} +

"$java_command" -jar "$stage/zt-server.jar" --version | grep -Fxq "$revision" || {
    echo 'Built jar reports an unexpected version' >&2; exit 1; }

rm -rf -- "$dist/linux-x64"
mv "$stage" "$dist/linux-x64"
trap - EXIT
tar -czf "$dist/linux-x64.tar.gz" -C "$dist/linux-x64" .

python3 .agents/skills/ztgui-build/scripts/package-server-sdk.py "$project_root"

if [[ "$voice" == ON ]]; then
    mvn -f src/server/pom.xml "-Drevision=$revision" "-DapiVersion=$api_version" \
        -pl api -am -DskipTests install
    mvn -f src/plugins/voice-chat/server/pom.xml "-DapiVersion=$api_version" clean package
    plugin_version="$(python3 - src/plugins/voice-chat/server/target/ztserver_voice_chat.jar <<'PY'
import json, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as jar:
    manifest = json.loads(jar.read('META-INF/ztserver-plugin.json'))
print(manifest['version'])
PY
)"
    mkdir -p "$dist/server-plugins"
    rm -f "$dist/server-plugins/"ztserver_voice_chat-v*.jar
    cp src/plugins/voice-chat/server/target/ztserver_voice_chat.jar \
        "$dist/server-plugins/ztserver_voice_chat-v$plugin_version.jar"
fi

echo 'Created build/dist/linux-x64.tar.gz (plugins published separately)'
