# ZTServer Java 迁移验证记录（2026-10-17）

迁移规格由用户提供（`GOAL.md`，任务完成后已按用户要求删除）。基线提交 `2018eca0920575bcc5af6f02e7cd48f6cfc4e915`，根 `VERSION=0.4.1`（未修改），API `0.4`。仓库中已有本轮之前的未提交改动；迁移只新增/修改服务端、服务端插件、测试、脚本与相关文档。

## 结论

ZTServer 已从 C++17/Qt6 迁移为 Java 25 + Netty 4.2 + Maven。旧项目、旧测试和旧 C++ 语音插件归档在 `src/server-old/`；新宿主、Java 插件 SDK、Java 语音插件、独立安装脚本、构建/打包脚本和文档完成。远端 `root@10.10.10.1` 已清理旧服务并部署新服务，ZeroTier、ztncui、MySQL/l4d2 容器和 3000/9993 端口未受影响。Ubuntu 22.04 容器可运行同一 fat jar（使用兼容 glibc 2.35 的 Azul JDK 25；Ubuntu 24.04 发行版 openjdk-25 包要求 glibc 2.38）。

## 产物

| 文件 | 大小 | SHA-256 |
| --- | ---: | --- |
| `build/dist/linux-x64/zt-server.jar` | 5,427,547 | `98a9eb84f06b9874a0d34665f760ec3cdd49242c6d59348c8a284e8be3467af2` |
| `build/dist/linux-x64.tar.gz` | 4,924,225 | `deb2e732cf0347724ed3fd340f09a2450c27c16273e72014ed1dd8396e9af338` |
| `build/dist/ZTServer-api-v0.4.zip` | 4,248,743 | `79dbe0f5d6f413651e7e7fe565139446a812056be321af9e32b8184b2569ea78` |
| `build/dist/server-plugins/ztserver_voice_chat-v0.1.0.jar` | 14,870 | `bd26389eab40876ec31169aa22ee92593ce208135d0c66717e0dbc722d785bad` |

`build/dist/linux-x64.tar.gz` 内容：`zt-server.jar`(644)、`start-server.sh`(755)、`zt-server.service`(644)、`zt-installer.sh`(755)、`README.md`(644)、`VERSION`(644)、`licenses/`(755 目录，文件 644)。SDK ZIP 含 jar/sources/javadoc/POM、`sample/`、`sdk.json`、`README.md`。

## 变更清单

- `src/server/`：Maven 父 POM、`api/` Java SDK、`main/` Java 25 + Netty 宿主、`scripts/` 启动/unit/安装脚本、`licenses/`、README/DEPLOYMENT。
- `src/server-old/`：原 C++/Qt6 服务端、SDK、测试、C++ 语音插件、旧构建脚本和归档 README。
- `src/plugins/voice-chat/server/`：Java Maven 语音插件（pom、manifest、`VoiceChatPlugin`、`VoiceProtocol`、JUnit、文档）。
- `src/tests/server/`：fixture 源码/构建脚本、IntegrationTests、StartupTests、InstallerTests、README；`src/tests/voice-chat/ServerTests.py` 支持 Java JAR 与部署模式。
- `scripts/build-linux.sh`、`.agents/skills/ztgui-build/scripts/build-linux.sh`、`package-server-sdk.py`、`sdk_packaging.py`、`SKILL.md`、`.agents/skills/ztgui-plugin-dev/SKILL.md`。
- 根 `CMakeLists.txt`（改为 `ZTGUI_BUILD_SERVER_OLD`）、`README.md`、`AGENTS.md`、`CHANGELOG.md`、`.gitignore`。
- `docs/server-api.md`、`network-relays.md`、`maintenance.md`、`plugin-runtime-separation.md`、`docs/validation/ztserver-java-2026-10-17/`。
- 产物：`build/dist/linux-x64.tar.gz`、`build/dist/linux-x64/`、`build/dist/ZTServer-api-v0.4.zip`、`build/dist/ZTServer-api-v0.4/`、`build/dist/server-plugins/ztserver_voice_chat-v0.1.0.jar`。

## 测试矩阵

| 编号 | 结果 | 证据 |
| --- | --- | --- |
| T1 Maven 编译/单测 | 通过 | Windows：8 API + 34 宿主测试；WSL 原生目录：8 API + 41 宿主 + 4 语音协议测试全部通过，`mvn ... clean verify` BUILD SUCCESS |
| T2 API 独立消费 | 通过 | WSL：把 SDK 的 `vusiif` 树复制到临时 `maven.repo.local`，`mvn -f build/dist/ZTServer-api-v0.4/sample/pom.xml package` 生成 `ztserver-api-sample-1.0.0.jar` |
| T3 TCP 路由 | 通过 | `IntegrationTests.py` 6/6：拆包/粘包、echo/other、未知/未就绪路由、非法帧、限速 |
| T4 UDP 路由 | 通过 | 同上：echo、未知/内置/超长/未就绪丢弃、后续 UDP 仍可用 |
| T5 builtin 查询 | 通过 | JUnit mock HTTP（token 头、HTTP 错误、JSON 形状、4 MiB 上限、取消、4 并发成员聚合、严格小写 networkId）；远端真实 `info/peers/networks/moons/controller.networks/controller.members` 全部 `ok`；日志无 token |
| T6 插件生命周期 | 通过 | IntegrationTests：echo、启动失败不 ready、取消/停止标记、重复 ID/错误 API/错误 mainClass/info 不一致拒绝、双写日志、数据目录；回调异常写入宿主日志 |
| T7 网络中转 | 通过 | `NetworkProtocolTest` 7 项 + `NetworkTransportTest` 7 项（fake controller、TCP RPC、UDP probe、register/relays/forward、授权、去重、发送 API）；远端 loopback `network.relays` 返回 `unauthorized` |
| T8 语音 | 通过 | 语音 JUnit `VoiceProtocolTest` 4/4；WSL 隔离 6/6；远端隔离 6/6；外部 `--host 10.10.10.1` 4 项通过、2 项按设计跳过 |
| T9 启动脚本 | 通过 | `StartupTests.py` 9/9（默认参数、参数/退出码、check、缺 Java/旧 Java/缺 JAR、JAVA_HOME）；端口就绪后立即/50ms 发送 SIGTERM，Linux 实测退出 0 |
| T10 安装脚本 | 通过 | `InstallerTests.py` 4/4（WSL root、沙箱路径和假命令） |
| T11 打包 | 通过 | WSL 原生目录 `build-linux.sh --build-voice-plugin`；权限、内容、版本、SDK ZIP CRC、sample 编译均检查 |
| T12 远端 24.04 | 通过 | 重建后重新部署：服务 active/enabled，TCP/UDP 18100 监听，真实查询通过，SIGTERM `Result=success`/`ExecMainStatus=0`，重启后 active；早期 SIGTERM（端口就绪后 0/0.05 秒）退出 0 |
| T13 Ubuntu 22.04 | 通过 | Docker `ubuntu:22.04`：`--version`=0.4.1，fixture echo 通过；使用 Azul JDK 25（宿主 Ubuntu 24.04 openjdk-25 需要 GLIBC 2.38，不能在 22.04 运行） |
| T14 旧项目 | 通过 | WSL（Qt6.4.2、cmake 3.28）完成 `cmake -S src/server-old -B /tmp/ztserver-old-config -DZTSERVER_BUILD_TESTS=ON -DZTSERVER_BUILD_VOICE_PLUGIN=ON`；`zt-server`、`server_package_tests`、`server_query_tests`、`ztserver_voice_chat_package` 均构建成功；CTest 2/2 通过 |
| T15 文档链接 | 通过 | 关键文档相对链接检查通过；构建入口已统一到 `.agents/skills`；网络传输实现路径指向实际源码目录 |

## 远端状态

- 迁移前：Ubuntu 24.04.4，旧 `zt-server.service` active，18100 TCP/UDP 监听；`ztgui-voice.service` inactive/disabled；`/opt/ztgui-voice` 存在；ZeroTier 9993、ztncui 3000、mysql/l4d2 容器运行。
- 备份：迁移前创建 `/root/Server/backups/`（ZTServer、/opt/ztgui-voice、systemd unit，时间戳 `20261017-194153`）。最终验证通过后按迁移规格清理，备份目录已删除；回滚需重新使用归档的 `src/server-old` 与旧 unit。
- JDK：Ubuntu 24.04 `openjdk-25-jdk` 25.0.4.1 安装到 `/usr/lib/jvm/java-25-openjdk-amd64`，`/opt/jdk-25` 软链、`/usr/local/bin/java|javac|jar` 软链；未采用 Temurin tarball，因为远端到 Adoptium/GitHub 下载持续超时，Ubuntu 仓库提供同主版本 JDK 25。Ubuntu 22.04 验证另外使用 Azul JDK 25.0.2（兼容 glibc 2.35），验证后已删除该临时 JDK 与容器镜像；第二台 Ubuntu 22.04 实机改用其官方源 `openjdk-25-jdk`。
- 清理：删除旧 unit、旧 `/root/Server/ZTServer` 内容、`/opt/ztgui-voice`；保留 `/var/lib/zerotier-one`、ZeroTier、ztncui 及其容器、mysql/l4d2 容器和 3000/9993 端口。
- 部署：新 unit 安装到 `/etc/systemd/system/zt-server.service` 并 enable；宿主包解压到 `/root/Server/ZTServer`；Java 语音插件复制到 `plugins/ztserver_voice_chat.jar`。
- 运行验证：`./start-server.sh --version`=0.4.1、`--check-deps` 通过；真实查询 6 项成功（节点 `5a3f1c4542`，网络 1 个，controller.members 14 个）；语音外部 4 项 + 隔离 6 项通过；日志无 `WARN/ERROR`、无 token；SIGTERM 正常退出 0（含端口就绪后立即/50ms 两次）；Ubuntu 22.04 容器 `--version` 和 echo 通过。
- 回滚：备份已验证可用前提，但最终验证通过后已删除；回滚需要重新部署归档的旧目录/unit，旧 C++ 包要求匹配的 Ubuntu 24.04 Qt/glibc 环境。
- 第二台 `175.27.165.250`（Ubuntu 22.04.5，工作目录 `/root/ZT`）：部署前备份并清空旧 C++ 包残留，安装 Ubuntu 22.04 源 `openjdk-25-jdk` 25.0.4.1，部署同一 `build/dist/linux-x64.tar.gz` 与语音 JAR；服务 active/enabled，真实查询、语音、SIGTERM 均通过；通过 ZeroTier `10.10.10.150:18100` 访问。详见 [第二台部署记录](second-host.md)。

## 未验证项

- 真实多人麦克风/耳机听感、跨地域语音质量未验收（协议与房间/UDP 隔离已自动化验证）。
- 旧 C++ 项目已在 WSL 构建宿主、测试与语音插件，CTest 2/2 通过；旧版 Python Integration/Startup/Installer 测试未在本轮重新执行（Phase 1 已运行旧 StartupTests 9/9）。
- 远端到 Adoptium/GitHub 的下载不通，因此 JDK 使用 Ubuntu 仓库包：第一台为 24.04 包，第二台为 22.04 源 `openjdk-25-jdk`；Docker 22.04 验证使用 Azul JDK 25。未实测 Temurin 官方 tarball。
- 真实网络中转多节点/多中转流量未在真实 ZeroTier 网络上联调；以 fake controller + 本机 UDP 单中转端到端与单元测试覆盖。
- 第二台公网 `18100` 被主机防火墙/安全组拦截，未修改防火墙；客户端通过 ZeroTier 地址访问。
- 未执行 `git commit`/`git push`，未修改根 `VERSION`。
