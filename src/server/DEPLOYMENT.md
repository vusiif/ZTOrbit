# ZTServer 运行与部署说明

运行平台：Linux（验证 Ubuntu 24.04 x86_64，并在 Ubuntu 22.04 容器验证同一 fat jar）。需要 **JDK 25 或更新版本**，发行包不内置 JRE。版本以包内 `VERSION`（服务端版本，来自客户端侧 `SERVER_VERSION`）或 `./start-server.sh --version` 为准。

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
systemctl edit zt-server.service
# [Service]
# Environment=JAVA_HOME=/opt/jdk-25
```

`start-server.sh` 优先用 `JAVA_HOME/bin/java`，否则用 `PATH` 中的 `java`。Java 缺失、版本低于 25 或 `zt-server.jar` 缺失时打印安装指引并以 **78** 退出；unit 的 `RestartPreventExitStatus=78` 阻止 systemd 反复重试配置错误。

## 2. 发行包与启动

`build/dist/linux-x64.tar.gz` 解压后包含 `zt-server.jar`、`start-server.sh`、`zt-server.service`、`zt-installer.sh`、`README.md`（本文件副本）、`VERSION`、`licenses/`。

```bash
tar -xzf linux-x64.tar.gz -C /root/Server/ZTServer
cd /root/Server/ZTServer
./start-server.sh --check-deps    # 仅检查，不启动
./start-server.sh                 # 默认 --server --listen 0.0.0.0 --port 18100
./start-server.sh --server --port 18200
./start-server.sh --version       # 0.2.1-beta
./start-server.sh --help
```

脚本固定以自身目录为工作目录；手动 `java -jar zt-server.jar ...` 时以调用者工作目录为准。

## 3. systemd

```bash
cd /root/Server/ZTServer
install -m644 zt-server.service /etc/systemd/system/zt-server.service
systemctl daemon-reload
systemctl enable --now zt-server.service
systemctl status zt-server.service
```

unit 的 `WorkingDirectory` 与 `ExecStart` 固定为 `/root/Server/ZTServer`，部署到其他目录时修改后 `daemon-reload`。SIGTERM/SIGINT 走 JVM shutdown hook，正常退出 0；`TimeoutStopSec=45`。默认监听全部 IPv4 的 TCP/UDP 18100；服务没有账户、Token 或 TLS，访问控制由部署网络/防火墙负责。

宿主日志在启动目录 `logs/YYMMDD-N.log`，一行一条 ISO 时间；插件日志同时写宿主日志和 `plugins-data/<插件显示名>/logs/`。更新程序时保留 `plugins-data/`、`logs/` 与 `relays.json`。

## 4. 插件 JAR

只加载 `plugins/` 直接子文件中的 `.jar`（非符号链接）。JAR 必须包含 `META-INF/ztserver-plugin.json`：

```json
{
  "formatVersion": 1,
  "host": "server",
  "id": "voice-chat",
  "name": "语音聊天",
  "version": "0.1.0",
  "apiVersion": "0.2-beta",
  "mainClass": "vusiif.ztgui.voicechat.VoiceChatPlugin"
}
```

- `apiVersion` 必须精确等于宿主 API（0.2-beta）；ID 匹配 `[A-Za-z0-9_-]{1,128}` 且不能为 `builtin`；显示名唯一并决定数据目录。
- 不再读取旧 `.ztplugin`，发现时记录 `unsupported package, use .jar`。
- 启动失败保留已加载对象但不 ready；增减插件后重启，不支持热加载。
- 语音 JAR 由 `bash scripts/build-linux.sh --build-voice-plugin` 生成到 `build/dist/server-plugins/`，复制到 `plugins/` 后重启。插件是可信代码，宿主不做沙箱或依赖隔离。

## 5. 安装/卸载/Moon（独立脚本）

| 旧命令 | 新命令 |
| --- | --- |
| `--install zerotier` | `bash zt-installer.sh --action 2` |
| `--install ztncui` | `bash zt-installer.sh --action 3` |
| `--install all` | `bash zt-installer.sh --action 1` |
| `--moon` | `bash zt-installer.sh --action 4` |
| `--uninstall ztncui` | `bash zt-installer.sh --action 5` |
| `--uninstall all` | `bash zt-installer.sh --action 6` |
| `--uninstall zerotier` | `bash zt-installer.sh --action 7` |

脚本保持原菜单/`--action` 行为：安装 ZeroTier 会生成或复用并导出 Moon；仅安装 ztncui 要求已有 ZeroTier；卸载 ZeroTier 删除节点身份；`--force` 重建 Moon/容器；`--ip` 指定公网 IPv4；仅支持 Ubuntu/Debian + systemd。它不删除无关数据，也不改动 ztncui 之外的服务。

## 6. 从旧 C++ 服务迁移与回滚

旧实现在 `src/server-old/`，旧 unit 与 `.ztplugin` 不再发布。

1. 准备维护窗口，备份旧运行目录、`/opt/ztgui-voice` 和 systemd unit。
2. 停止并禁用旧 `zt-server.service`/`ztgui-voice.service`，确认 18100/18101 空闲。
3. 清理旧目录内容，安装新包与 unit，`systemctl enable --now zt-server.service`。
4. 放入 Java 语音插件 JAR 后重启；客户端协议未改，无需更新客户端。
5. 保持 ZeroTier、ztncui、Moon 与容器数据不变。

回滚：停止/禁用新服务，恢复旧目录与旧 unit，`daemon-reload` 后启用旧 `zt-server.service` 和 `ztgui-voice.service`。旧 C++ 包需要匹配的 Ubuntu 24.04 Qt/glibc 环境，Ubuntu 22.04 不能直接运行。

## 7. 故障排查

- `--check-deps` 失败退出 78：按提示安装 JDK 25 或修正 `JAVA_HOME`。
- 端口占用：`ss -ltnup | grep -E ':18100|:18101'`，处理后重启。
- 查询错误：确认 ZeroTier 运行且 `/var/lib/zerotier-one/authtoken.secret` 可读；日志不会输出 token。
- 插件未加载：检查 JAR 位置、manifest 的 `apiVersion`/`id`/`name`，以及宿主日志中的 `Plugin rejected ...` 原因。
- 语音：确认 `plugins-data/语音聊天/settings.json` 合法，18100 UDP 可达。

测试与验证记录见 `docs/validation/ztserver-java-2026-10-17/`。
