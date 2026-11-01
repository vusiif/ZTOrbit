# ZTOrbitHub（Java 25 + Netty）

Linux 无界面 ZeroTier 只读查询与插件转发服务。宿主负责 TCP/UDP 路由、`builtin` 查询、Java 插件加载和网络中转；ZeroTier/ztncui 安装、卸载与 Moon 导出由独立 `scripts/z2-installer.sh` 完成。

部署支持范围以[网络部署约定](../../docs/network-conventions.md)为准：本项目仅支持符合约定的部署，约定外配置由部署者负责。

- 运行时：Java 25（不内置 JRE），Netty 4.2.x NIO。
- 构建：Maven 3.9+，`maven.compiler.release=25`。
- 版本：服务端程序与 API 由客户端侧维护的根 `SERVER_VERSION` 指定；程序 `0.2.1`，插件 API `0.2`。客户端 `VERSION` 不再决定服务端版本。
- 旧 C++ 实现归档在 [`../server-old/`](../server-old/README.md)。

## 目录

```text
src/server/
  pom.xml             聚合父 POM
  api/                Java 插件 SDK（ztohub-api，版本=${apiVersion}）
  main/               宿主实现与 fat jar（Z2Hub.jar）
  scripts/            start-server.sh、z2hub.service、z2-installer.sh
  licenses/           Apache-2.0、Netty NOTICE、Jackson NOTICE
```

## 构建

```bash
revision="$(tr -d '\r\n' < SERVER_VERSION)"
api_version="$(printf '%s' "$revision" | sed -E 's/^0\.([0-9]+)\.[0-9]+(-[0-9A-Za-z.-]+)?$/0.\1\2/')"
mvn -f src/server/pom.xml -Drevision="$revision" -DapiVersion="$api_version" clean verify
```

产物为 `src/server/main/target/Z2Hub.jar`。发布版本必须由脚本传入；直接构建子模块会使用开发默认版本 `0.0.0-dev`/`0.0`。

Linux 发行打包：

```bash
bash scripts/build-linux.sh                        # build/dist/Z2Hub-linux-x64.tar.gz + build/dist/Z2Hub-api-v0.2.zip
bash scripts/build-linux.sh --build-voice-plugin   # 另生成 build/dist/server-plugins/z2hub_voice_chat-v0.1.0.jar
```

## 运行

```bash
cd /root/Server/Z2Hub
./start-server.sh --version
./start-server.sh --check-deps
./start-server.sh                 # 默认 --server --listen 0.0.0.0 --port 18100
```

CLI 保留 `--server`、`--listen <IPv4>`、`--port <1-65535>`、`--version`、`--help`。旧安装参数（`--install/-i`、`--uninstall/-u`、`--moon/-m`、`--ip`、`--force`、`--yes`、`--dry-run`）提示使用 `./z2-installer.sh` 并以 2 退出。

项目约定的服务端口为 18100；改用其他端口属于约定外配置，配套调整与排查由部署者负责。

服务保持前台运行，由 `scripts/z2hub.service` 托管。启动目录决定 `plugins/`、`plugins-data/` 和 `logs/` 的位置；安装、插件、数据与回滚见 [DEPLOYMENT.md](DEPLOYMENT.md)。

## Java 插件

插件是 `plugins/*.jar`，内含 `META-INF/ztohub-plugin.json` 并实现 `vusiif.ztohub.api.ServerPlugin`。仅启动时扫描，不支持热加载。SDK 签名、`ZTS1`/`builtin` 协议、网络中转和线程约定见 [服务端 API](../../docs/server-api.md)。

语音服务端插件位于 [`src/plugins/voice-chat/server`](../plugins/voice-chat/server/docs/voice-chat.md)，与客户端 `ZTV1` 协议保持字节兼容。

## 验证

测试位于 `src/tests/server/` 与 `src/tests/voice-chat/`。
