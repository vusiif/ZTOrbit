#!/bin/bash
set -euo pipefail

# Resolve paths before any check so invocation from another directory works.
server_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
jar="$server_dir/Z2Hub.jar"
check_only=false
if [[ "${1:-}" == --check-deps ]]; then
    if (( $# != 1 )); then
        echo '用法: start-server.sh --check-deps（仅检查，不接受服务参数）' >&2
        exit 64
    fi
    check_only=true
fi

java_help() {
    cat >&2 <<'HELP'

需要 JDK 25 或更高版本（本发行包不内置 JRE）。Ubuntu 22.04 / 24.04 安装示例：
  sudo apt-get update
  sudo apt-get install -y wget apt-transport-https gpg
  sudo install -d -m 755 /etc/apt/keyrings
  wget -qO- https://packages.adoptium.net/artifactory/api/gpg/key/public \
    | sudo gpg --dearmor -o /etc/apt/keyrings/adoptium.gpg
  . /etc/os-release
  echo "deb [signed-by=/etc/apt/keyrings/adoptium.gpg] https://packages.adoptium.net/artifactory/deb $VERSION_CODENAME main" \
    | sudo tee /etc/apt/sources.list.d/adoptium.list >/dev/null
  sudo apt-get update
  sudo apt-get install -y temurin-25-jdk

验证：java -version 应显示 25 或更高版本。
HELP
}

repository_failure() {
    printf '运行依赖检查失败，服务未启动：%s\n' "$1" >&2
    java_help
    exit 78
}

# Use JAVA_HOME when it points at a real JDK; otherwise use PATH.
java_command=''
if [[ -n "${JAVA_HOME:-}" && -x "$JAVA_HOME/bin/java" ]]; then
    java_command="$JAVA_HOME/bin/java"
elif command -v java >/dev/null 2>&1; then
    java_command="$(command -v java)"
fi
if [[ -z "$java_command" ]]; then
    repository_failure '找不到 Java 运行时'
fi

java_version="$("$java_command" -version 2>&1 || true)"
java_major="$(printf '%s\n' "$java_version" | sed -n 's/.*version "\([0-9][0-9]*\).*/\1/p' | head -1)"
if [[ -z "$java_major" ]]; then
    repository_failure "无法识别 Java 版本：$java_version"
fi
if (( java_major < 25 )); then
    repository_failure "需要 JDK 25 或更高版本，当前主版本为 $java_major"
fi
if [[ ! -f "$jar" || ! -r "$jar" ]]; then
    repository_failure "找不到可读的服务端程序 $jar；请完整解压发行包"
fi

if "$check_only"; then
    echo "依赖检查通过：Java $java_major，$jar"
    exit 0
fi

# Keep plugins, plugins-data and logs beside the server, including manual invocations.
cd -- "$server_dir"
if (( $# == 0 )); then
    set -- --server --listen 0.0.0.0 --port 18100
fi
# Preserve signals and the server's exit status for systemd.
exec "$java_command" -jar "$jar" "$@"
