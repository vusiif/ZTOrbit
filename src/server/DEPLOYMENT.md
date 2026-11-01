# ZTOrbitHub 运行与部署说明

运行平台：Linux（验证 Ubuntu 24.04 x86_64，并在 Ubuntu 22.04 容器验证同一 fat jar）。需要 **JDK 25 或更新版本**，发行包不内置 JRE。版本以包内 `VERSION`（服务端版本，来自客户端侧 `SERVER_VERSION`）或 `./start-server.sh --version` 为准。

部署支持范围以[网络部署约定](../../docs/network-conventions.md)为准：本项目仅支持符合约定的部署，约定外配置由部署者负责。

## 1. 安装 JDK 25

Ubuntu 22.04 / 24.04 使用 Temurin 25（root）：

```bash
apt-get update
apt-get install -y wget apt-transport-https gpg
install -d -m 755 /etc/apt/keyrings
wget -qO- https://packages.adoptium.net/artifactory/api/gpg/key/public \
  | gpg --dearmor -o /etc/apt/keyrings/adoptium.gpg
. /etc/os-release
echo "deb [signed-by=/etc/apt/keyrings/adoptium.gpg] https://packages.adoptium.net/artifactory/deb $VERSION_CODENAME main" \
  | tee /etc/apt/sources.list.d/adoptium.list >/dev/null
apt-get update && apt-get install -y temurin-25-jdk
java -version
```

离线 tarball 可解压到 `/opt/jdk-25`；systemd 的 `PATH` 不包含 Java 时增加 drop-in：

```bash
systemctl edit z2hub.service
# [Service]
# Environment=JAVA_HOME=/opt/jdk-25
```

`start-server.sh` 优先用 `JAVA_HOME/bin/java`，否则用 `PATH` 中的 `java`。Java 缺失、版本低于 25 或 `Z2Hub.jar` 缺失时打印安装指引并以 **78** 退出；unit 的 `RestartPreventExitStatus=78` 阻止 systemd 反复重试配置错误。

## 2. 发行包与启动

`build/dist/Z2Hub-linux-x64.tar.gz` 解压后包含 `Z2Hub.jar`、`start-server.sh`、`z2hub.service`、`z2-installer.sh`、`README.md`（本文件副本）、`VERSION`、`licenses/`。

```bash
tar -xzf Z2Hub-linux-x64.tar.gz -C /root/Server/Z2Hub
cd /root/Server/Z2Hub
./start-server.sh --check-deps    # 仅检查，不启动
./start-server.sh                 # 默认 --server --listen 0.0.0.0 --port 18100
./start-server.sh --version       # 版本取自根 SERVER_VERSION
./start-server.sh --help
```

脚本固定以自身目录为工作目录；手动 `java -jar Z2Hub.jar ...` 时以调用者工作目录为准。

CLI 仍提供 `--port <1-65535>`，项目约定使用 18100；使用其他端口属于约定外配置，配套调整与排查由部署者负责。

## 3. systemd

```bash
cd /root/Server/Z2Hub
install -m644 z2hub.service /etc/systemd/system/z2hub.service
systemctl daemon-reload
systemctl enable --now z2hub.service
systemctl status z2hub.service
```

unit 的 `WorkingDirectory` 与 `ExecStart` 固定为 `/root/Server/Z2Hub`，部署到其他目录时修改后 `daemon-reload`。SIGTERM/SIGINT 走 JVM shutdown hook，正常退出 0；`TimeoutStopSec=45`。默认监听全部 IPv4 的 TCP/UDP 18100；服务没有账户、Token 或 TLS，访问控制由部署网络/防火墙负责。

宿主日志在启动目录 `logs/YYMMDD-N.log`，一行一条 ISO 时间；插件日志同时写宿主日志和 `plugins-data/<插件显示名>/logs/`。更新程序时保留 `plugins-data/`、`logs/` 与 `relays.json`。

## 4. 插件 JAR

只加载 `plugins/` 直接子文件中的 `.jar`（非符号链接）。JAR 必须包含 `META-INF/ztohub-plugin.json`：

```json
{
  "formatVersion": 1,
  "host": "server",
  "id": "voice-chat",
  "name": "语音聊天",
  "version": "0.1.0",
  "apiVersion": "0.2",
  "mainClass": "vusiif.ztohub.voicechat.VoiceChatPlugin"
}
```

- `apiVersion` 必须精确等于宿主 API（0.2）；ID 匹配 `[A-Za-z0-9_-]{1,128}` 且不能为 `builtin`；显示名唯一并决定数据目录。
- 不再读取旧 `.z2plugin`，发现时记录 `unsupported package, use .jar`。
- 启动失败保留已加载对象但不 ready；增减插件后重启，不支持热加载。
- 语音 JAR 由 `bash scripts/build-linux.sh --build-voice-plugin` 生成到 `build/dist/server-plugins/`，复制到 `plugins/` 后重启。插件是可信代码，宿主不做沙箱或依赖隔离。

## 5. 安装/卸载/Moon（独立脚本）

| 旧命令 | 新命令 |
| --- | --- |
| `--install zerotier` | `bash z2-installer.sh --action 2` |
| `--install ztncui` | `bash z2-installer.sh --action 3` |
| `--install all` | `bash z2-installer.sh --action 1` |
| `--moon` | `bash z2-installer.sh --action 5` |
| `--uninstall ztncui` | `bash z2-installer.sh --action 6` |
| `--uninstall all` | `bash z2-installer.sh --action 8` |
| `--uninstall zerotier` | `bash z2-installer.sh --action 9` |

全量安装（选项 1）包含 ZeroTier、Moon、ztncui 和 pylon。菜单选项 **4「仅安装pylon」** 也可直接运行 `sudo bash z2-installer.sh --action 4`；后续选项为 5「导出moon文件」、6「仅卸载ztncui」、7「仅卸载pylon」、8「卸载」，仅命令行可用的「仅卸载ZeroTier」为 `--action 9`。仅安装 pylon 只准备 Docker 并部署 pylon，无需本机安装 ZeroTier，也不拉取 ztncui 镜像。pylon 使用以下命令，容器名为 `ztpylon`，以 `reflect` 模式运行：

```bash
docker run --name ztpylon --init -d -p 9443:443 zerotier/pylon:latest reflect
```

请在云安全组放行 **9443/TCP**（pylon）与 **9993/UDP**（ZeroTier）。ZeroTier 本地管理 API 也会监听 **9993/TCP**，因此同机部署时不能将 pylon 的宿主端口映射到 9993；pylon 默认使用 9443/TCP 映射容器的 443/TCP，与[官方 Docker 示例](https://github.com/zerotier/pylon#docker)的 TCP 映射一致。

部署前应确保约定的 9443/TCP 可用。脚本仍支持 `--pylon-port <端口>`（也支持 `--pylon-port=<端口>`），取值限于 1–65535；改用其他端口属于约定外配置，防火墙、安全组和客户端 `tcpFallbackRelay` 地址的配套调整与排查由部署者负责。脚本先检查 TCP 端口，发现冲突会在删除或创建容器前退出。

相同端口下，已运行的 pylon 容器会跳过创建，已停止的容器会启动。若部署者自行更改运行过的容器端口，脚本要求使用 `--force --pylon-port <新端口>` 重建。上次因端口绑定失败留下的 `created` 状态容器若映射不同端口，会在新端口预检通过后自动重建；新的 Docker 启动失败若留下未启动容器，会自动清理，便于再次执行。

选项 **7「仅卸载pylon」**（`sudo bash z2-installer.sh --action 7`）删除 `ztpylon` 容器与 `zerotier/pylon:latest` 镜像，保留 ZeroTier、Moon 和 ztncui。选项 **8「卸载」** 会清理 ztncui、pylon 的容器和镜像、ztncui 配置，再卸载 ZeroTier 及其数据目录；Docker 不可用或容器/镜像清理失败时停止操作并报错。选项 6 仅卸载 ztncui，`--action 9` 仅卸载 ZeroTier，均保留 pylon。交互菜单中的卸载操作需要二次确认；`--action` 模式直接执行。

安装 ZeroTier 会生成或复用并导出 Moon；仅安装 ztncui 要求已有 ZeroTier；卸载 ZeroTier 删除节点身份；`--force` 还会重建 Moon；`--ip` 指定公网 IPv4；仅支持 Ubuntu/Debian + systemd。

## 6. 从旧 C++ 服务迁移与回滚

旧实现在 `src/server-old/`，旧 unit 与 `.z2plugin` 不再发布。

1. 准备维护窗口，备份旧运行目录、`/opt/zto-voice` 和 systemd unit。
2. 停止并禁用旧 `z2hub.service`/`zto-voice.service`，确认 18100/18101 空闲。
3. 清理旧目录内容，安装新包与 unit，`systemctl enable --now z2hub.service`。
4. 放入 Java 语音插件 JAR 后重启；客户端协议未改，无需更新客户端。
5. 保持 ZeroTier、ztncui、Moon 与容器数据不变。

回滚：停止/禁用新服务，恢复旧目录与旧 unit，`daemon-reload` 后启用旧 `z2hub.service` 和 `zto-voice.service`。旧 C++ 包需要匹配的 Ubuntu 24.04 Qt/glibc 环境，Ubuntu 22.04 不能直接运行。

## 7. 故障排查

- `--check-deps` 失败退出 78：按提示安装 JDK 25 或修正 `JAVA_HOME`。
- 端口占用：`ss -ltnup | grep -E ':18100|:18101'`，处理后重启。
- 查询错误：确认 ZeroTier 运行且 `/var/lib/zerotier-one/authtoken.secret` 可读；日志不会输出 token。
- 插件未加载：检查 JAR 位置、manifest 的 `apiVersion`/`id`/`name`，以及宿主日志中的 `Plugin rejected ...` 原因。
- 语音：确认 `plugins-data/语音聊天/settings.json` 合法，18100 UDP 可达。

测试位于 `src/tests/server/` 与 `src/tests/voice-chat/`。
