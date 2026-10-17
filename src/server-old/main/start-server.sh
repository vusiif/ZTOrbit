#!/bin/bash
set -euo pipefail

# Resolve paths before checking libraries so invocation from another directory works.
server_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
server="$server_dir/zt-server"
check_only=false
if [[ "${1:-}" == --check-deps ]]; then
    if (( $# != 1 )); then
        echo '用法: start-server.sh --check-deps（仅检查，不接受服务参数）' >&2
        exit 64
    fi
    check_only=true
fi

if [[ ! -f "$server" || ! -x "$server" ]]; then
    echo "错误：找不到可执行程序 $server；请完整解压发行包并检查执行权限。" >&2
    exit 78
fi
if ! command -v ldd >/dev/null 2>&1; then
    echo '错误：缺少 ldd，无法检查动态运行库。Ubuntu 请先安装 libc-bin。' >&2
    exit 78
fi

dependency_status=0
dependencies="$(LC_ALL=C ldd "$server" 2>&1)" || dependency_status=$?
if (( dependency_status != 0 )) || [[ "$dependencies" == *'not found'* ]]; then
    printf '运行依赖检查失败，服务未启动：\n%s\n' "$dependencies" >&2
    ID=unknown
    VERSION_ID=unknown
    if [[ -r /etc/os-release ]]; then
        . /etc/os-release
    fi
    printf '\n当前系统：%s %s\n' "$ID" "$VERSION_ID" >&2
    if [[ "$ID" == ubuntu && "$VERSION_ID" == 24.04 ]]; then
        cat >&2 <<'HELP'
请以 root 执行以下命令，完成后重新运行启动脚本：
  apt-get update
  apt-get install -y libqt6core6t64 libqt6network6t64 libsodium23
找不到 Qt6 软件包时，请检查是否启用了 Ubuntu universe 仓库。
HELP
    else
        echo '此发行包针对 Ubuntu 24.04 x86_64 构建；请使用匹配的系统或在目标环境重新构建。' >&2
    fi
    echo '若输出含 GLIBC、GLIBCXX 或 Qt 版本不匹配，请使用匹配的构建，不要手工创建库软链接。' >&2
    exit 78
fi

if "$check_only"; then
    echo '运行依赖检查通过。'
    exit 0
fi

# Keep plugins and runtime data beside the server, including manual invocations.
cd -- "$server_dir"
if (( $# == 0 )); then
    set -- --server --listen 0.0.0.0 --port 18100
fi
# Preserve signals and the server's exit status for systemd.
exec "$server" "$@"
