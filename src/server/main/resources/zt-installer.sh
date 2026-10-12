#!/usr/bin/env bash
# ============================================================================
# ZeroTier + ztncui 菜单式部署脚本
#
# 参考文档：zts.md
#
# 执行后先列出数字选项，再按选择执行：
#   1) 安装            全量安装：ZeroTier + moon + ztncui，并导出 moon 文件
#   2) 仅安装ZeroTier  安装 ZeroTier、构建 moon，并导出 moon 文件
#   3) 仅安装ztncui    部署 ztncui（本机无 ZeroTier 则不做任何事）
#   4) 导出moon文件    构建（若缺失）并导出 moon 文件到当前目录
#   5) 仅卸载ztncui    删除 ztncui 容器、镜像与配置目录
#   6) 卸载            删除 ztncui 全部内容，再卸载 ZeroTier 及数据目录
#
# 设计要点：
#   - 前置条件先行：构建 moon 之前先确认 $MOON_PORT/UDP 可用；部署 ztncui
#     之前先确认本机 ZeroTier 就绪（选项 3 不会代装 ZeroTier）；
#   - moon 文件导出到「执行脚本时所在的目录」($RUN_DIR)，脚本内部不改 cwd；
#   - 幂等：已存在 moon / 容器时跳过，--force 才重建；
#   - 卸载（选项 6）会删除节点身份，节点 ID 将不可恢复。
#
# 适用环境：Ubuntu Server 24.04 / Debian 系（RPM 系仅卸载做了兼容），需 root
# 使用示例：
#   sudo bash zt-installer.sh                      # 菜单模式
#   sudo bash zt-installer.sh --action 2 --ip 1.2.3.4
#   sudo bash zt-installer.sh --action 6           # 全量卸载
# ============================================================================

set -euo pipefail
TEMP_DIR="$(mktemp -d)"
trap 'rm -rf -- "$TEMP_DIR"' EXIT
PRESERVE_ZTNCUI=0

# ============================== 参数定义 ==============================

PUBLIC_IP=""                        # 服务器公网 IP：留空则自动检测，可用 --ip 指定
FORCE=0                             # 1 = 强制重建 moon 节点与 ztncui 容器
ACTION=""                           # 菜单编号："1"~"6"，"0" = 退出；为空则显示菜单
INTERACTIVE=0                       # 1 = 操作来自菜单（需要二次确认的破坏性操作）

RUN_DIR="$(pwd)"                    # 执行脚本时所在的目录：moon 文件导出到这里

ZT_HOME="/var/lib/zerotier-one"     # ZeroTier 数据目录
MOON_PORT="9993"                    # ZeroTier 默认 UDP 端口，moon stableEndpoints 也用它

ZTNCUI_CONTAINER="ztncui"           # ztncui 容器名
ZTNCUI_IMAGE="keynetworks/ztncui"   # ztncui 官方镜像
ZTNCUI_HTTP_PORT="3000"             # ztncui HTTP 访问端口

EXPORTED_MOON=""                    # 本次导出到 $RUN_DIR 的 moon 文件绝对路径

# ztncui 管理员密码文件内容（与 zts.md 一致：账号 admin / 密码 adminpassword）
# hash 字段为 argon2i 哈希；首次登录后请立即修改密码
ZTNCUI_PASSWD_JSON='{"admin":{"name":"admin","pass_set":false,"hash":"$argon2i$v=19$m=4096,t=3,p=1$vYiXnH7UOBwIyiNXfW0nZg$1GFEtcKZ/S/UgkosNb1RTfOCQrdKJbdjcRnp+H3Hv9g"}}'

# ============================== 工具函数 ==============================

info() { printf '\033[32m[信息]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[警告]\033[0m %s\n' "$*"; }
step() { printf '\n\033[36m===== %s =====\033[0m\n' "$*"; }
die()  { printf '\033[31m[错误]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<EOF
用法: sudo bash $(basename "$0") [选项]

不带选项运行时，先列出数字操作菜单，再按所选编号执行：
  1 安装            全量安装（ZeroTier + moon + ztncui），并导出 moon 文件
  2 仅安装ZeroTier  安装 ZeroTier、构建 moon，并导出 moon 文件
  3 仅安装ztncui    部署 ztncui（要求本机已安装 ZeroTier，否则不做任何事）
  4 导出moon文件    构建（若缺失）并导出 moon 文件到当前目录
  5 仅卸载ztncui    删除 ztncui 容器、镜像与配置目录
  6 卸载            删除 ztncui 全部内容，再卸载 ZeroTier 及数据目录

选项:
  --action <0-6>  跳过菜单直接执行指定操作（0 = 退出）
                  该模式下 5、6 不再二次确认，请自行确认意图
  --ip <IPv4>     手动指定服务器公网 IP（构建/导出 moon 时使用）
  --force         重建 moon 文件、删除重建 ztncui 容器
  -h, --help      显示本帮助

moon 文件导出位置：执行脚本时所在的目录（$RUN_DIR）
EOF
}

# 读取用户输入：优先 /dev/tty，避免脚本被管道执行（curl | bash）时把脚本内容当输入
read_tty() {
  if [[ -r /dev/tty ]]; then
    read -r "$@" < /dev/tty
  else
    read -r "$@"
  fi
}

# 校验是否为合法 IPv4 地址
is_ipv4() {
  local ip="$1" n
  [[ "$ip" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || return 1
  local IFS='.'
  for n in $ip; do
    (( n <= 255 )) || return 1
  done
  return 0
}

# 自动检测公网 IP：优先 cip.cc（文档所用），失败后依次尝试备用接口
# 结果通过标准输出返回，进度信息输出到标准错误（避免混入结果）
detect_public_ip() {
  local url ip
  for url in "https://cip.cc" "https://ipv4.icanhazip.com" "https://api.ipify.org"; do
    printf '[信息] 尝试通过 %s 检测公网 IP ...\n' "$url" >&2
    ip="$(curl -s -m 10 --connect-timeout 5 "$url" 2>/dev/null | grep -oE '([0-9]{1,3}\.){3}[0-9]{1,3}' | head -n1 || true)"
    if is_ipv4 "$ip"; then
      printf '%s' "$ip"
      return 0
    fi
  done
  return 1
}

# 确定公网 IP：优先 --ip 参数，其次自动检测
resolve_public_ip() {
  if [[ -z "$PUBLIC_IP" ]]; then
    PUBLIC_IP="$(detect_public_ip)" || die "公网 IP 自动检测失败，请使用 --ip <IPv4> 手动指定后重试"
  fi
  is_ipv4 "$PUBLIC_IP" || die "公网 IP 不合法：$PUBLIC_IP"
  info "服务器公网 IP：$PUBLIC_IP"
}

# 检测 UDP 端口是否处于监听状态
# 返回：0 = 已监听；1 = 未监听；2 = 系统缺少 ss/netstat，无法判断
# 注意：这里不用管道接 grep -q（pipefail 下可能因 SIGPIPE 误判），改用 here-string
udp_port_listening() {
  local port="$1" out=""
  if command -v ss >/dev/null 2>&1; then
    out="$(ss -lun 2>/dev/null || true)"
  elif command -v netstat >/dev/null 2>&1; then
    out="$(netstat -lun 2>/dev/null || true)"
  else
    return 2
  fi
  grep -qE "[:.]${port}[[:space:]]" <<< "$out"
}

# 输出监听指定 UDP 端口的进程名（去重、空格分隔；无法获知时输出空字符串）
udp_port_holders() {
  local port="$1" raw="" out=""
  if command -v ss >/dev/null 2>&1; then
    # 从 users:(("进程名",pid=...)) 中取出进程名（去掉首尾引号）
    raw="$(ss -lunp 2>/dev/null || true)"
    out="$(grep -E "[:.]${port}[[:space:]]" <<< "$raw" \
      | grep -oE 'users:\(\("[^"]+"' | sed -e 's/^.*("//' -e 's/"$//' \
      | sort -u | tr '\n' ' ' || true)"
  elif command -v netstat >/dev/null 2>&1; then
    # 末列为 PID/进程名；无权限时显示为 -，需要过滤掉
    raw="$(netstat -lunp 2>/dev/null || true)"
    out="$(grep -E "[:.]${port}[[:space:]]" <<< "$raw" \
      | awk '{print $NF}' | awk -F'/' '{print $NF}' | grep -vE '^-$' \
      | sort -u | tr '\n' ' ' || true)"
  fi
  printf '%s' "$out"
}

# 等待 ZeroTier 监听 $MOON_PORT/UDP（最长约 30 秒）
wait_zt_port() {
  for _ in {1..15}; do
    if udp_port_listening "$MOON_PORT"; then
      return 0
    fi
    sleep 2
  done
  return 1
}

# 预检：$MOON_PORT/UDP 是否被其他进程占用
# 该端口决定了 moon 的 stableEndpoints 能否被客户端连上，被占用时继续做下去没有意义
precheck_zt_port() {
  if ! command -v ss >/dev/null 2>&1 && ! command -v netstat >/dev/null 2>&1; then
    warn "未找到 ss/netstat 命令，无法检测 $MOON_PORT/UDP 端口状态（建议安装 iproute2）"
    return 0
  fi
  if ! udp_port_listening "$MOON_PORT"; then
    info "$MOON_PORT/UDP 当前空闲，可供 ZeroTier 使用"
    return 0
  fi
  local holders
  holders="$(udp_port_holders "$MOON_PORT")"
  if [[ -z "$holders" ]]; then
    warn "$MOON_PORT/UDP 已被监听，但无法识别占用进程；若 ZeroTier 未监听该端口，请检查 $ZT_HOME/local.conf"
  elif [[ "$holders" == *zerotier-one* ]]; then
    info "$MOON_PORT/UDP 当前由 zerotier-one 监听"
  else
    die "$MOON_PORT/UDP 已被其他进程占用：${holders%% }
请先停止该进程，或改用其他端口（需同步修改脚本中的 MOON_PORT 与 moon 的 stableEndpoints）后重试"
  fi
}

# 校验 local.conf 里的 primaryPort 是否与脚本预期一致
# 该值会覆盖默认的 9993，不一致时 moon 的 stableEndpoints 与 ztncui 的 ZT_ADDR 都会失效
check_zt_port_config() {
  [[ -f "$ZT_HOME/local.conf" ]] || return 0
  grep -q '"primaryPort"' "$ZT_HOME/local.conf" || return 0
  local actual_port
  actual_port="$(grep -oE '"primaryPort"[[:space:]]*:[[:space:]]*[0-9]+' "$ZT_HOME/local.conf" \
    | grep -oE '[0-9]+$' | head -n1 || true)"
  if [[ -n "$actual_port" && "$actual_port" != "$MOON_PORT" ]]; then
    die "$ZT_HOME/local.conf 中的 primaryPort 为 $actual_port，与脚本预期的 $MOON_PORT 不一致。
请修改 $ZT_HOME/local.conf 使用 $MOON_PORT，或将脚本中的 MOON_PORT 改为 $actual_port 后重试"
  fi
}

# 找到本机当前生效的 moon 文件（优先 moons.d，其次待移动的 000000*.moon）
current_moon_file() {
  local f=""
  if [[ -d "$ZT_HOME/moons.d" ]]; then
    f="$(ls -t "$ZT_HOME"/moons.d/*.moon 2>/dev/null | head -n1 || true)"
  fi
  if [[ -z "$f" ]]; then
    f="$(ls -t "$ZT_HOME"/000000*.moon 2>/dev/null | head -n1 || true)"
  fi
  printf '%s' "$f"
}

# ufw 已启用时放行端口（参数形如 9993/udp），未启用则静默跳过
ufw_allow() {
  local port_proto="$1" desc="$2"
  command -v ufw >/dev/null 2>&1 || return 0
  ufw status 2>/dev/null | grep -q "Status: active" || return 0
  if ufw allow "$port_proto" >/dev/null 2>&1; then
    info "ufw：已放行 $port_proto（$desc）"
  else
    warn "ufw：放行 $port_proto 失败，请手动执行：ufw allow $port_proto"
  fi
  return 0
}

# ============================== 参数解析 ==============================

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ip)
      [[ -n "${2:-}" ]] || die "--ip 需要提供 IPv4 地址"
      is_ipv4 "$2" || die "--ip 参数不是合法 IPv4 地址：$2"
      PUBLIC_IP="$2"
      shift 2
      ;;
    --ip=*)
      PUBLIC_IP="${1#*=}"
      is_ipv4 "$PUBLIC_IP" || die "--ip 参数不是合法 IPv4 地址：$PUBLIC_IP"
      shift
      ;;
    --action)
      [[ -n "${2:-}" ]] || die "--action 需要提供编号（0-6）"
      ACTION="$2"
      shift 2
      ;;
    --action=*)
      ACTION="${1#*=}"
      shift
      ;;
    --force)
      FORCE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "未知参数：$1（使用 -h 查看帮助）"
      ;;
  esac
done

if [[ -n "$ACTION" && ! "$ACTION" =~ ^[0-7]$ ]]; then
  die "--action 只接受 0-7：${ACTION}"
fi

# ============================== 菜单 ==============================

show_menu() {
  cat <<EOF

==================== ZeroTier + ztncui 部署脚本 ====================
导出目录：$RUN_DIR

  1) 安装            全量安装：ZeroTier + moon + ztncui，并导出 moon 文件
  2) 仅安装ZeroTier  安装 ZeroTier、构建 moon，并导出 moon 文件（不装 Docker/ztncui）
  3) 仅安装ztncui    部署 ztncui 控制台（要求本机已安装 ZeroTier，否则不做任何事）
  4) 导出moon文件    构建（若缺失）并导出 moon 文件到导出目录（要求本机已安装 ZeroTier）
  5) 仅卸载ztncui    删除 ztncui 容器、镜像与配置目录（ZeroTier 不受影响）
  6) 卸载            删除 ztncui 全部内容，再卸载 ZeroTier 与数据目录

  0) 退出
==================================================================
EOF
}

# 读取菜单选择，写入 ACTION
choose_action() {
  local choice
  while true; do
    show_menu
    printf '请输入选项编号 [0-6]: '
    if ! read_tty choice; then
      printf '\n' >&2
      die "无法读取输入（当前环境非交互），请改用：bash $0 --action <编号>"
    fi
    case "$choice" in
      0|1|2|3|4|5|6)
        ACTION="$choice"
        return 0
        ;;
      "")
        warn "输入为空，请重新选择"
        ;;
      *)
        warn "无效选项：$choice（请输入 0-6 之间的数字）"
        ;;
    esac
  done
}

# 破坏性操作的二次确认（仅交互模式调用）
confirm_action() {
  local answer=""
  case "$1" in
    5)
      printf '\033[33m[确认]\033[0m 即将删除：ztncui 容器、镜像，以及配置目录 %s\n' "$ZT_HOME/ztncui"
      printf '         ZeroTier 本体不受影响。\n'
      ;;
    6)
      printf '\033[33m[确认]\033[0m 即将删除：ztncui 容器、镜像、配置目录，卸载 ZeroTier 软件包，\n'
      printf '         并删除数据目录 %s（节点身份、moon 文件都在其中）。\n' "$ZT_HOME"
      printf '         \033[33m节点 ID 将不可恢复，重装后是全新节点。\033[0m\n'
      ;;
    *)
      return 0
      ;;
  esac
  printf '确认继续？[y/N] '
  if ! read_tty answer; then
    return 1
  fi
  [[ "$answer" == "y" || "$answer" == "Y" || "$answer" == "yes" || "$answer" == "YES" ]]
}

# ============================== 操作：安装 ZeroTier ==============================

base_env_checks() {
  if [[ -f /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    info "当前系统：${PRETTY_NAME:-未知}"
  fi
  command -v curl >/dev/null 2>&1 || die "缺少 curl 命令，请先安装：apt-get update && apt-get install -y curl"
  command -v systemctl >/dev/null 2>&1 || die "未检测到 systemd，本脚本仅支持 systemd 系统"
}

# 安装 ZeroTier 并确认 $MOON_PORT/UDP 可用（幂等）
ensure_zerotier() {
  step "安装 ZeroTier 并确认 $MOON_PORT/UDP 可用"

  if command -v zerotier-cli >/dev/null 2>&1; then
    info "ZeroTier 已安装，跳过安装步骤"
  else
    precheck_zt_port
    info "下载 ZeroTier 官方安装脚本 ..."
    curl -fsSL -m 60 --connect-timeout 10 https://install.zerotier.com -o $TEMP_DIR/zerotier-install.sh \
      || die "下载 ZeroTier 安装脚本失败，请检查网络后重试"

    info "执行安装（可能需要 1-2 分钟）..."
    bash $TEMP_DIR/zerotier-install.sh || die "ZeroTier 安装失败"
    info "ZeroTier 安装完成"
  fi

  # 确保服务已启用并运行
  systemctl enable --now zerotier-one >/dev/null 2>&1 || true

  # 等待服务初始化（生成身份文件与 API token）
  for _ in {1..15}; do
    if [[ -f "$ZT_HOME/identity.public" && -f "$ZT_HOME/authtoken.secret" ]]; then
      break
    fi
    sleep 2
  done
  [[ -f "$ZT_HOME/identity.public" ]] || die "ZeroTier 未生成身份文件，请检查服务状态：systemctl status zerotier-one"
  [[ -f "$ZT_HOME/authtoken.secret" ]] || die "未找到 $ZT_HOME/authtoken.secret，请检查服务状态：systemctl status zerotier-one"

  ZERO_ID="$(zerotier-cli info 2>/dev/null | awk '{print $3}' || true)"
  info "ZeroTier 节点 ID：${ZERO_ID:-未知}"

  check_zt_port_config

  # 确认 ZeroTier 已监听 $MOON_PORT/UDP
  info "等待 ZeroTier 监听 $MOON_PORT/UDP ..."
  if ! wait_zt_port; then
    local holders
    holders="$(udp_port_holders "$MOON_PORT")"
    if [[ -n "$holders" ]]; then
      warn "$MOON_PORT/UDP 被以下进程占用：$holders"
    fi
    warn "zerotier-one 服务最近日志："
    journalctl -u zerotier-one -n 20 --no-pager 2>/dev/null | sed 's/^/    /' || true
    die "ZeroTier 未监听 $MOON_PORT/UDP，moon 节点无法正常工作。
排查方向：端口被占用 / $ZT_HOME/local.conf 配置异常 / 服务启动失败"
  fi
  info "ZeroTier 已监听 $MOON_PORT/UDP"

  # 放行防火墙（放行的正是 moon 要用的端口）
  ufw_allow "$MOON_PORT/udp" "ZeroTier"

  # 等待节点上线（ONLINE 表示已能与根服务器通信；失败不阻塞后续步骤）
  local zt_online=0
  for _ in {1..15}; do
    if [[ "$(zerotier-cli info 2>/dev/null | awk '{print $5}' || true)" == "ONLINE" ]]; then
      zt_online=1
      break
    fi
    sleep 2
  done
  if (( zt_online )); then
    info "ZeroTier 节点状态：ONLINE"
  else
    warn "ZeroTier 节点尚未 ONLINE（可能是出站 UDP 受限或网络延迟），可稍后执行 zerotier-cli info 复查"
  fi
}

# 要求本机已安装 ZeroTier（选项 3/4 使用）：不会代装 ZeroTier
require_zerotier() {
  step "检查本机 ZeroTier"
  if ! command -v zerotier-cli >/dev/null 2>&1; then
    die "本机未安装 ZeroTier，已按要求跳过该操作。
请先执行选项 2（仅安装 ZeroTier）或选项 1（全量安装）后再试"
  fi
  [[ -f "$ZT_HOME/authtoken.secret" ]] \
    || die "未找到 $ZT_HOME/authtoken.secret，ZeroTier 可能未完成初始化：systemctl status zerotier-one"

  if ! systemctl is-active --quiet zerotier-one; then
    info "ZeroTier 服务未运行，正在启动 ..."
    systemctl enable --now zerotier-one >/dev/null 2>&1 || true
    sleep 2
    systemctl is-active --quiet zerotier-one || die "ZeroTier 服务启动失败：systemctl status zerotier-one"
  fi

  check_zt_port_config
  info "本机 ZeroTier 已就绪（节点 ID：$(zerotier-cli info 2>/dev/null | awk '{print $3}' || echo 未知)）"
}

# ============================== 操作：构建 / 导出 moon ==============================

# 构建 moon 节点（幂等：已存在则跳过，--force 才重建）
build_moon() {
  step "构建 moon 节点"

  command -v zerotier-idtool >/dev/null 2>&1 || die "未找到 zerotier-idtool 命令，请确认 ZeroTier 已正确安装"
  [[ -f "$ZT_HOME/identity.public" ]] || die "未找到 $ZT_HOME/identity.public，ZeroTier 尚未完成初始化"

  if (( ! FORCE )) && ls "$ZT_HOME"/moons.d/*.moon >/dev/null 2>&1; then
    info "检测到已有 moon 文件，跳过构建（需要重建请加 --force 参数）"
    return 0
  fi

  precheck_zt_port
  resolve_public_ip

  # 生成 moon.json
  local moon_json="$ZT_HOME/moon.json"
  zerotier-idtool initmoon "$ZT_HOME/identity.public" > "$moon_json"
  info "已生成 $moon_json"

  # 写入 stableEndpoints（文档要求：公网IP/9993，即 $MOON_PORT）
  # 优先用 python3 做结构化修改；python3 不存在或执行失败（例如某些环境里的
  # 空壳 shim）则回退到 sed 替换，下面的 grep 校验是两条路径的共同兜底
  local endpoints_set=0
  if command -v python3 >/dev/null 2>&1; then
    if python3 - "$PUBLIC_IP" "$MOON_PORT" "$moon_json" <<'PY'
import json
import sys

ip, port, path = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path, encoding="utf-8") as f:
    moon = json.load(f)

moon["roots"][0]["stableEndpoints"] = [ip + "/" + port]

with open(path, "w", encoding="utf-8") as f:
    json.dump(moon, f, indent=2)
PY
    then
      endpoints_set=1
    else
      warn "python3 修改 moon.json 失败，改用 sed 替换"
    fi
  fi
  if (( ! endpoints_set )); then
    sed -i "s|\"stableEndpoints\"[[:space:]]*:[[:space:]]*\[[^]]*\]|\"stableEndpoints\": [\"$PUBLIC_IP/$MOON_PORT\"]|" "$moon_json"
  fi
  grep -q "$PUBLIC_IP/$MOON_PORT" "$moon_json" \
    || die "moon.json 中未成功写入 stableEndpoints，请手动编辑 $moon_json 后重试"
  info "stableEndpoints 已设置为：[\"$PUBLIC_IP/$MOON_PORT\"]"

  # 生成签名 moon 文件（文件名格式：000000 + 节点ID + .moon）
  # genmoon 会把文件生成到当前目录，因此在子 shell 中切到 $ZT_HOME 执行
  ( cd "$ZT_HOME" && zerotier-idtool genmoon moon.json ) || die "genmoon 生成 moon 文件失败"
  local moon_file
  moon_file="$(ls -t "$ZT_HOME"/000000*.moon 2>/dev/null | head -n1 || true)"
  [[ -n "$moon_file" ]] || die "genmoon 未生成 .moon 文件，请检查上方命令输出"
  info "已生成 moon 文件：$(basename "$moon_file")"

  # 移动到 moons.d 目录（--force 时先清理旧文件，避免残留）
  mkdir -p "$ZT_HOME/moons.d"
  if (( FORCE )); then
    rm -f "$ZT_HOME"/moons.d/*.moon
  fi
  mv -f "$moon_file" "$ZT_HOME/moons.d/"
  info "moon 文件已移动到 $ZT_HOME/moons.d/"

  # 重启 ZeroTier 服务使 moon 生效
  systemctl restart zerotier-one
  sleep 2
  info "ZeroTier 服务已重启"

  # 重启后复查端口（此时才真正用上先前校验的端口）
  if udp_port_listening "$MOON_PORT"; then
    info "重启后 $MOON_PORT/UDP 监听正常"
  else
    warn "重启后未检测到 $MOON_PORT/UDP 监听，请执行：systemctl status zerotier-one"
  fi
}

# 把当前 moon 文件复制到 $RUN_DIR（执行脚本时所在的目录）
export_moon_file() {
  step "导出 moon 文件"

  local src dest
  src="$(current_moon_file)"
  [[ -n "$src" && -f "$src" ]] \
    || die "未找到 moon 文件（$ZT_HOME/moons.d/ 为空），请先执行选项 1 或 2 构建 moon"

  dest="$RUN_DIR/$(basename "$src")"
  if [[ -f "$dest" ]]; then
    info "目标位置已存在同名文件，将覆盖：$dest"
  fi
  cp -f "$src" "$dest" || die "复制 moon 文件到 $dest 失败（目录是否可写？）"
  EXPORTED_MOON="$dest"

  # sudo 场景下把文件交还给调用者，避免导出后普通用户无法复制/编辑
  if [[ -n "${SUDO_USER:-}" && "${SUDO_USER:-}" != "root" ]]; then
    chown "$SUDO_USER":"$(id -gn "$SUDO_USER" 2>/dev/null || printf '%s' "$SUDO_USER")" "$dest" 2>/dev/null || true
  fi

  info "moon 文件已导出：$dest"
}

# ============================== 操作：安装 / 卸载 ztncui ==============================

# 安装 Docker 并预拉取 ztncui 镜像（幂等）
ensure_docker() {
  step "准备 Docker 环境"

  if command -v docker >/dev/null 2>&1; then
    info "Docker 已安装：$(docker --version)"
  else
    info "未检测到 Docker，开始安装 ..."
    curl -fsSL -m 60 --connect-timeout 10 https://get.docker.com -o $TEMP_DIR/get-docker.sh \
      || die "下载 Docker 安装脚本失败，请检查网络后重试"
    sh $TEMP_DIR/get-docker.sh || die "Docker 安装失败"
    info "Docker 安装完成"
  fi

  systemctl enable --now docker >/dev/null 2>&1 || true

  # 等待 Docker 守护进程就绪（刚安装或刚启动时可能需数秒）
  local docker_ready=0
  for _ in {1..15}; do
    if docker info >/dev/null 2>&1; then
      docker_ready=1
      break
    fi
    sleep 2
  done
  (( docker_ready )) || die "Docker 服务未正常运行，请检查：systemctl status docker"
  info "Docker 服务运行正常"

  # 预拉取 ztncui 镜像：把「镜像仓库是否可达」这一风险提前暴露
  if docker image inspect "$ZTNCUI_IMAGE" >/dev/null 2>&1; then
    info "ztncui 镜像已存在：$ZTNCUI_IMAGE"
  else
    info "预拉取 ztncui 镜像（首次可能需要几分钟）..."
    docker pull "$ZTNCUI_IMAGE" \
      || die "镜像拉取失败：请检查服务器能否访问 Docker Hub，或配置镜像加速器后重试"
    info "ztncui 镜像拉取完成"
  fi
}

# 部署 ztncui 容器（幂等：已运行则跳过，--force 才重建）
deploy_ztncui() {
  step "部署 ztncui"

  # 1) 创建管理员密码文件
  # 注意：zts.md 中的 /opt/key-networks/ztncui/etc/passwd 是【容器内】路径，
  # 经下面的 -v 挂载后，宿主机上对应的实际路径为 $ZT_HOME/ztncui/passwd
  mkdir -p "$ZT_HOME/ztncui"
  if [[ -f "$ZT_HOME/ztncui/passwd" ]]; then
    info "密码文件已存在，保留现有配置（如需重置为默认密码，删除 $ZT_HOME/ztncui/passwd 后重跑）"
  else
    printf '%s\n' "$ZTNCUI_PASSWD_JSON" > "$ZT_HOME/ztncui/passwd"
    info "已创建管理员密码文件（账号 admin / 密码 adminpassword，与 zts.md 一致）"
  fi

  # 2) 创建 / 重建容器
  if docker ps -a --format '{{.Names}}' | grep -qx "$ZTNCUI_CONTAINER"; then
    if (( FORCE )); then
      info "--force 模式：删除旧容器并重建 ..."
      docker rm -f "$ZTNCUI_CONTAINER" >/dev/null
    elif docker ps --format '{{.Names}}' | grep -qx "$ZTNCUI_CONTAINER"; then
      info "容器 $ZTNCUI_CONTAINER 已在运行，跳过创建（需要重建请加 --force 参数）"
    else
      info "容器 $ZTNCUI_CONTAINER 已存在但未运行，正在启动 ..."
      docker start "$ZTNCUI_CONTAINER" >/dev/null
    fi
  fi

  if ! docker ps --format '{{.Names}}' | grep -qx "$ZTNCUI_CONTAINER"; then
    # 镜像已预拉取，这里只做兜底（例如被外部清理过）
    if ! docker image inspect "$ZTNCUI_IMAGE" >/dev/null 2>&1; then
      info "镜像不存在，重新拉取 ztncui 镜像 ..."
      docker pull "$ZTNCUI_IMAGE" \
        || die "镜像拉取失败：请检查服务器能否访问 Docker Hub，或配置镜像加速器后重试"
    fi

    info "启动 ztncui 容器 ..."
    # 说明：zts.md 中的 -itd 适用于终端交互场景；脚本环境无 TTY，改用 -d 后台运行
    docker run -d \
      --name "$ZTNCUI_CONTAINER" \
      --restart=always \
      --network=host \
      -v "$ZT_HOME/authtoken.secret:/var/lib/zerotier-one/authtoken.secret" \
      -v "$ZT_HOME/ztncui:/opt/key-networks/ztncui/etc" \
      -e HTTP_ALL_INTERFACES=yes \
      -e ZT_ADDR="127.0.0.1:$MOON_PORT" \
      --entrypoint /start_ztncui.sh \
      "$ZTNCUI_IMAGE" \
      || die "ztncui 容器启动失败"

    sleep 3
    if ! docker ps --format '{{.Names}}' | grep -qx "$ZTNCUI_CONTAINER"; then
      warn "容器未保持运行状态，最近日志如下："
      docker logs --tail 50 "$ZTNCUI_CONTAINER" 2>&1 || true
      die "ztncui 容器启动异常，请根据上方日志排查"
    fi
    info "ztncui 容器已启动"
  fi

  # 3) 放行控制台端口（建议仅对可信 IP 开放）
  ufw_allow "$ZTNCUI_HTTP_PORT/tcp" "ztncui 控制台"

  # 4) 等待 HTTP 就绪（最长约 60 秒）
  info "等待 ztncui 就绪 ..."
  local ready=0
  for _ in {1..30}; do
    if curl -s -o /dev/null -m 3 "http://127.0.0.1:$ZTNCUI_HTTP_PORT/"; then
      ready=1
      break
    fi
    sleep 2
  done
  if (( ready )); then
    info "ztncui 已就绪（本机 $ZTNCUI_HTTP_PORT 端口可访问）"
  else
    warn "ztncui 端口暂未就绪，可查看日志排查：docker logs --tail 100 $ZTNCUI_CONTAINER"
  fi
}

# 卸载 ztncui：容器 + 镜像 + 配置目录（ZeroTier 本体不动）
uninstall_ztncui() {
  step "卸载 ztncui"

  # 容器与镜像
  if ! command -v docker >/dev/null 2>&1; then
    info "未安装 Docker，跳过容器与镜像清理"
  else
    if ! docker info >/dev/null 2>&1; then
      info "Docker 服务未运行，尝试启动 ..."
      systemctl enable --now docker >/dev/null 2>&1 || true
      for _ in {1..10}; do
        if docker info >/dev/null 2>&1; then
          break
        fi
        sleep 2
      done
    fi

    if ! docker info >/dev/null 2>&1; then
      warn "Docker 服务不可用，无法删除容器与镜像。服务恢复后请手动执行：
    docker rm -f $ZTNCUI_CONTAINER
    docker rmi -f $ZTNCUI_IMAGE"
      return 1
    else
      if docker ps -a --format '{{.Names}}' | grep -qx "$ZTNCUI_CONTAINER"; then
        if docker rm -f "$ZTNCUI_CONTAINER" >/dev/null 2>&1; then
          info "已删除容器：$ZTNCUI_CONTAINER"
        else
          warn "删除容器失败，请手动执行：docker rm -f $ZTNCUI_CONTAINER"
          return 1
        fi
      else
        info "未发现容器 $ZTNCUI_CONTAINER（跳过）"
      fi

      if docker image inspect "$ZTNCUI_IMAGE" >/dev/null 2>&1; then
        if docker rmi -f "$ZTNCUI_IMAGE" >/dev/null 2>&1; then
          info "已删除镜像：$ZTNCUI_IMAGE"
        else
          warn "删除镜像失败（可能仍被其他容器引用），请手动执行：docker rmi -f $ZTNCUI_IMAGE"
          return 1
        fi
      else
        info "未发现镜像 $ZTNCUI_IMAGE（跳过）"
      fi
    fi
  fi

  # 配置目录（含密码文件）
  if [[ -d "$ZT_HOME/ztncui" ]]; then
    if rm -rf "$ZT_HOME/ztncui"; then
      info "已删除配置目录：$ZT_HOME/ztncui"
    else
      warn "删除配置目录失败，请手动执行：rm -rf $ZT_HOME/ztncui"; return 1
    fi
  else
    info "未发现配置目录 $ZT_HOME/ztncui（跳过）"
  fi
}

# ============================== 操作：卸载 ZeroTier ==============================

uninstall_zerotier() {
  step "卸载 ZeroTier"

  # 1) 停止并禁用服务
  if systemctl list-unit-files 2>/dev/null | grep -q '^zerotier-one'; then
    systemctl stop zerotier-one >/dev/null 2>&1 || true
    systemctl disable zerotier-one >/dev/null 2>&1 || true
    info "已停止并禁用 zerotier-one 服务"
  else
    info "未发现 zerotier-one 服务（可能已卸载）"
  fi

  # 2) 卸载软件包（apt / dnf / yum）
  if command -v apt-get >/dev/null 2>&1 && dpkg -s zerotier-one >/dev/null 2>&1; then
    info "卸载软件包 zerotier-one（apt-get remove）..."
    if DEBIAN_FRONTEND=noninteractive apt-get remove -y zerotier-one >/dev/null 2>&1; then
      info "软件包 zerotier-one 已卸载"
    else
      warn "apt-get remove 失败，请手动执行：apt-get remove -y zerotier-one"; return 1
    fi
  elif command -v dnf >/dev/null 2>&1 && rpm -q zerotier-one >/dev/null 2>&1; then
    info "卸载软件包 zerotier-one（dnf remove）..."
    if dnf remove -y zerotier-one >/dev/null 2>&1; then
      info "软件包 zerotier-one 已卸载"
    else
      warn "dnf remove 失败，请手动执行：dnf remove -y zerotier-one"; return 1
    fi
  elif command -v yum >/dev/null 2>&1 && rpm -q zerotier-one >/dev/null 2>&1; then
    info "卸载软件包 zerotier-one（yum remove）..."
    if yum remove -y zerotier-one >/dev/null 2>&1; then
      info "软件包 zerotier-one 已卸载"
    else
      warn "yum remove 失败，请手动执行：yum remove -y zerotier-one"; return 1
    fi
  else
    info "未发现 zerotier-one 软件包（可能已卸载）"
  fi

  # 3) 删除数据目录（节点身份、moon 文件、ztncui 配置都在其中）
  if [[ -d "$ZT_HOME" ]]; then
    if { if (( PRESERVE_ZTNCUI )); then
      find "$ZT_HOME" -mindepth 1 -maxdepth 1 ! -name ztncui -exec rm -rf -- {} +
    else
      rm -rf -- "$ZT_HOME"
    fi; }; then
      if (( PRESERVE_ZTNCUI )); then
        info "已删除 ZeroTier 数据，保留 $ZT_HOME/ztncui 配置"
      else
        info "已删除数据目录：$ZT_HOME（节点身份、moon、ztncui 配置）"
      fi
    else
      warn "删除数据目录失败，请手动执行：rm -rf $ZT_HOME"; return 1
    fi
  else
    info "未发现数据目录 $ZT_HOME（跳过）"
  fi

  # 4) 清理安装时添加的软件源
  local f
  for f in /etc/apt/sources.list.d/zerotier.list \
           /usr/share/keyrings/zerotier-debian-package-key.gpg \
           /etc/yum.repos.d/zerotier.repo; do
    if [[ -e "$f" ]]; then
      if rm -f "$f"; then
        info "已删除软件源文件：$f"
      else
        warn "删除失败：$f"
        return 1
      fi
    fi
  done

  # 5) 复查
  if command -v zerotier-cli >/dev/null 2>&1; then
    warn "zerotier-cli 仍然存在，可能未完全卸载，请检查：dpkg -l | grep -i zerotier"
    return 1
  else
    info "ZeroTier 已卸载完成"
  fi
}

# ============================== 各菜单操作 ==============================

action_full_install() {
  step "操作 1：全量安装"
  ensure_zerotier
  build_moon
  export_moon_file
  ensure_docker
  require_zerotier
  deploy_ztncui
  show_summary install
}

action_zerotier_only() {
  step "操作 2：仅安装 ZeroTier"
  ensure_zerotier
  build_moon
  export_moon_file
  show_summary zerotier
}

action_ztncui_only() {
  step "操作 3：仅安装 ztncui"
  # 按要求：本机没有 ZeroTier 就什么都不做（不会代装 ZeroTier）
  require_zerotier
  ensure_docker
  deploy_ztncui
  show_summary ztncui
}

action_export_moon() {
  step "操作 4：导出 moon 文件"
  require_zerotier
  build_moon          # moon 缺失时会自动构建（需要公网 IP）
  export_moon_file
  show_summary export
}

action_uninstall_ztncui() {
  step "操作 5：仅卸载 ztncui"
  uninstall_ztncui
  show_summary uninstall-ztncui
}

action_uninstall_all() {
  step "操作 6：卸载（ztncui + ZeroTier）"
  uninstall_ztncui
  uninstall_zerotier
  show_summary uninstall
}

# ============================== 结果汇总 ==============================

print_moon_usage() {
  local p="${EXPORTED_MOON:-$(current_moon_file)}"
  [[ -n "$p" ]] || return 0
  cat <<EOF

moon 文件：
  位置：$p
  客户端用法：把该文件放入客户端的 moons.d 目录后重启 ZeroTier 服务
    Linux:   /var/lib/zerotier-one/moons.d/
    Windows: C:\\ProgramData\\ZeroTier\\One\\moons.d\\
EOF
}

print_security_notes() {
  cat <<EOF

注意事项：
  1. 默认密码（admin / adminpassword）是公开的，请首次登录后立即修改
  2. 云服务器请在控制台安全组放行：$MOON_PORT/UDP（ZeroTier 必需）、
     $ZTNCUI_HTTP_PORT/TCP（ztncui 控制台，建议仅对可信 IP 开放）
     本脚本只能确认「本机监听 + 本机防火墙放行」，公网可达性取决于云安全组
EOF
}

print_uninstall_notes() {
  cat <<EOF

说明：
  moon 文件（$RUN_DIR 下）未被删除，如需清理请自行删除
  重装后可执行选项 2 重新构建 moon（节点身份已重置，节点 ID 会变化）
EOF
}

show_summary() {
  local mode="$1"
  local zt_state zt_port moon_state ztncui_state ztncui_access="" moons=""

  step "当前状态"

  # ZeroTier
  if command -v zerotier-cli >/dev/null 2>&1; then
    zt_state="$(zerotier-cli info 2>/dev/null | sed 's/^/  /' || true)"
    [[ -n "$zt_state" ]] || zt_state="  已安装，但无法获取状态（可执行 zerotier-cli info 复查）"
    moons="$(zerotier-cli listmoons 2>/dev/null || true)"
  else
    zt_state="  未安装"
  fi

  # $MOON_PORT/UDP
  if ! command -v zerotier-cli >/dev/null 2>&1; then
    zt_port="未安装（不适用）"
  elif udp_port_listening "$MOON_PORT"; then
    zt_port="已监听 $MOON_PORT/UDP"
  else
    zt_port="未监听到 $MOON_PORT/UDP（请检查：systemctl status zerotier-one）"
  fi

  # moon
  if ! command -v zerotier-cli >/dev/null 2>&1; then
    moon_state="  未安装"
  elif [[ -n "$moons" ]]; then
    moon_state="$(printf '%s\n' "$moons" | sed 's/^/  /')"
  else
    moon_state="  （无，可执行选项 2 或 4 构建）"
  fi

  # ztncui
  if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    if docker ps --format '{{.Names}}' | grep -qx "$ZTNCUI_CONTAINER"; then
      ztncui_state="运行中（容器 $ZTNCUI_CONTAINER）"
      if [[ -z "$PUBLIC_IP" ]]; then
        PUBLIC_IP="$(detect_public_ip 2>/dev/null || true)"
      fi
      ztncui_access="
  访问地址：http://${PUBLIC_IP:-<服务器公网IP>}:$ZTNCUI_HTTP_PORT/
  登录账号：admin    登录密码：adminpassword"
    elif docker ps -a --format '{{.Names}}' | grep -qx "$ZTNCUI_CONTAINER"; then
      ztncui_state="已停止（容器 $ZTNCUI_CONTAINER，可执行 docker start $ZTNCUI_CONTAINER）"
    else
      ztncui_state="未部署"
    fi
  else
    ztncui_state="未部署（Docker 未安装或未运行）"
  fi

  cat <<EOF

==================== 当前状态 ====================
ZeroTier：
$zt_state
$MOON_PORT/UDP：$zt_port

moon：
$moon_state

ztncui：
  状态：$ztncui_state$ztncui_access
==================================================
EOF

  case "$mode" in
    install|zerotier|export) print_moon_usage ;;
  esac
  case "$mode" in
    install|zerotier|ztncui) print_security_notes ;;
    uninstall-ztncui|uninstall) print_uninstall_notes ;;
  esac
}

# ============================== 入口 ==============================

run_action() {
  case "$ACTION" in
    0)
      info "已退出，未做任何修改"
      exit 0
      ;;
  esac

  [[ $EUID -eq 0 ]] || die "请以 root 权限运行：sudo bash $0"
  base_env_checks
  [[ -f /etc/os-release ]] || die "Missing /etc/os-release"
  . /etc/os-release
  [[ "${ID:-}" == ubuntu || "${ID:-}" == debian || " ${ID_LIKE:-} " == *" debian "* ]] || die "Only Ubuntu/Debian installation workflows are supported"
  [[ ! -L "$ZT_HOME" ]] || die "ZeroTier data directory must not be a symbolic link"

  case "$ACTION" in
    1) action_full_install ;;
    2) action_zerotier_only ;;
    3) action_ztncui_only ;;
    4) action_export_moon ;;
    5) action_uninstall_ztncui ;;
    6) action_uninstall_all ;;
    7) PRESERVE_ZTNCUI=1; uninstall_zerotier; show_summary uninstall ;;
    *) die "未知操作编号：$ACTION（应为 0-7）" ;;
  esac
}

if [[ -z "$ACTION" ]]; then
  choose_action
  INTERACTIVE=1
fi

# 破坏性操作在交互选择时二次确认（--action 模式视为已确认）
if (( INTERACTIVE )) && { [[ "$ACTION" == "5" ]] || [[ "$ACTION" == "6" ]]; }; then
  if ! confirm_action "$ACTION"; then
    info "已取消，未做任何修改"
    exit 0
  fi
fi

run_action
