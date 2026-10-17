# 第二台服务器部署记录（175.27.165.250）

时间：2026-10-17。目标由用户通过 `.tmp/temp-ssh-key/server.json` 指定：

```json
{"ip":"175.27.165.250","workspace":"/root/ZT"}
```

## 部署前状态

- 系统 Ubuntu 22.04.5 LTS x86_64，root；磁盘约 8.1 GiB 可用，内存 3.6 GiB。
- 工作目录 `/root/ZT` 中只有旧的 C++ ZTServer 包残留：`zt-server`、旧 `start-server.sh`、旧 `README.md`/`VERSION`/`zt-server.service`、`ztserver_voice_chat.ztplugin` 和 `linux-x64.tar.gz`。
- 没有 `zt-server.service`/`ztgui-voice.service`，18100 未监听；`zerotier-one` active，ztncui 3000 监听，容器 `ztncui`/`mysql`/`anne` 运行。
- 系统没有 Java；Ubuntu 22.04 源提供 `openjdk-25-jdk` 25.0.4.1+1~22.04.4。

## 清理与安装

1. 备份旧目录到 `/root/ZT-old-20261017-220336.tar.gz`（验证成功后删除）。
2. 清空 `/root/ZT` 内容并恢复为 `root:root 755`；未触碰 ZeroTier、ztncui、容器和 3000/9993。
3. `apt-get install -y openjdk-25-jdk` 安装 Ubuntu 22.04 原生 JDK 25（`/usr/lib/jvm/java-25-openjdk-amd64`，`/usr/bin/java`），随后 `apt-get clean`。
4. 上传并解压同一 `build/dist/linux-x64.tar.gz`（sha256 `deb2e732...`），安装语音插件 `plugins/ztserver_voice_chat.jar`（sha256 `bd26389e...`）。
5. 安装 unit 到 `/etc/systemd/system/zt-server.service`，将模板中的默认目录 `/root/Server/ZTServer` 改为本机工作目录 `/root/ZT`；`enable --now`。

```ini
WorkingDirectory=/root/ZT
ExecStart=/root/ZT/start-server.sh --server --listen 0.0.0.0 --port 18100
```

## 验证结果

- 服务 active (running)、enabled；TCP/UDP 18100 监听；`./start-server.sh --version` = `0.4.1`；`--check-deps` 通过。
- 真实只读查询：`info`/`peers`/`networks`/`moons`/`controller.networks` 成功；节点 `0f1229adc6`；本机控制网络 `0f1229adc6c861e5` 的 `controller.members` 成功（2 个成员）。`5a3f1c454238ae4a` 的 `controller.members` 返回 HTTP 404，因为本机不是该公共网络的控制器，属于预期。
- 语音：从本机（Windows，ZeroTier 地址 `10.10.10.3`）连接 `10.10.10.150:18100`，外部 4 项通过、2 项按设计跳过；在目标机隔离模式运行 6/6 通过。
- SIGTERM：服务停止后 `Result=success`、`ExecMainStatus=0`，重启后 active；端口就绪后立即/50ms 发送 SIGTERM 均退出 0。
- 受保护资源：`zerotier-one` active、9993 监听，ztncui 3000 监听，`ztncui`/`mysql`/`anne` 容器均 Up。

## 访问路径与注意事项

- 公网 `175.27.165.250:18100` 被本机 `YJ-FIREWALL-INPUT` 拒绝（SSH 22 可通）；未修改防火墙/安全组，避免把无认证的 ZTServer 暴露到公网。客户端应通过 ZeroTier 地址 `10.10.10.150:18100` 访问。
- 目标机 ZeroTier 还有一个本机未实际使用的网络 `0f1229adc6c861e5`（controller 报告 `24.35.46.1/24`，但本机没有该接口）；网络中转会对该网络记录 `Network socket bind failed ... Cannot assign requested address` 告警并跳过，不影响主网络、查询和语音。可在 ZeroTier 中移除该未使用网络或恢复其接口以消除告警。
- 旧备份和 `/tmp` 上传文件在验证通过后删除；`/root/ZT` 保留新服务和 `logs/`、`plugins-data/`。

## 产物

与第一台服务器相同：

- `build/dist/linux-x64.tar.gz` — sha256 `deb2e732cf0347724ed3fd340f09a2450c27c16273e72014ed1dd8396e9af338`
- `build/dist/server-plugins/ztserver_voice_chat-v0.1.0.jar` — sha256 `bd26389eab40876ec31169aa22ee92593ce208135d0c66717e0dbc722d785bad`
