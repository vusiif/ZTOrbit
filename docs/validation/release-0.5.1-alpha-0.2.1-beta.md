# 0.5.1-alpha / 0.2.1-beta 构建与验证（2026-10-18）

客户端 `0.5.1-alpha`（客户端 API `0.5-alpha`），服务端 `0.2.1-beta`（服务端 API `0.2-beta`）。客户端版本来自根 `VERSION`，服务端版本来自客户端侧维护的根 `SERVER_VERSION`。归档 C++ 服务端保持 `0.4.1`／API `0.4`。

## 环境

- 客户端：Windows 11 x64、MSVC 2022 19.44、动态 Qt 6.8.3。
- 服务端：WSL Ubuntu 24.04、OpenJDK 25.0.2、Maven 3.9.9。
- 媒体：Qt 6.8.3、FFmpeg 7.1.5、OpenH264 2.6.0、Opus 1.6.1。
- 根 `VERSION=0.5.1-alpha`，`SERVER_VERSION=0.2.1-beta`，`src/server-old/VERSION=0.4.1`。

## 产物

完整大小与 SHA-256 见 [产物记录](release-0.5.1-alpha-0.2.1-beta-artifacts.json)。

- 客户端：`build/dist/windows-x64.zip`（191 个文件）、`build/dist/ZTGUI-api-v0.5-alpha.zip`。
- 服务端：`build/dist/linux-x64.tar.gz`、`build/dist/ZTServer-api-v0.2-beta.zip`。
- 媒体：`build/dist/ZTGUI-media-api-v2.zip`、`build/dist/ZTGUI-media-engine-sdk-v1.zip`、`build/dist/plugins/ztgui_media_worker-v2.0.0.ztplugin`。
- 客户端语音：`build/dist/plugins/ztgui_voice_chat-v0.3.0.ztplugin`。
- Java 语音服务端：`build/dist/server-plugins/ztserver_voice_chat-v0.1.0.jar`（manifest `apiVersion=0.2-beta`）。

## 验证结果

| 检查 | 结果 |
| --- | --- |
| 客户端 CTest（Windows x64） | 10/10 通过；包含 GUI、GPU 参数、核心、动态插件、多中转、插件服务。 |
| 媒体 worker 集成 | 通过（约 70 秒）；worker 使用包内 Qt／媒体后端／FFmpeg 运行库。 |
| 客户端语音插件面板 | 通过；按设计跳过 3 个需要外部服务器或独立预览包的用例。测试目录需补齐 Qt 运行库，属于开发环境布置，不是插件问题。 |
| 服务端 Maven | `ZTServer 0.2.1-beta`、API `0.2-beta` reactor 构建成功；宿主 41 项测试、API 测试通过。 |
| Java 语音服务端插件 | 4 项 `VoiceProtocolTest` 通过；JAR manifest `apiVersion=0.2-beta`。 |
| 服务端 Linux 包 | 包内 `VERSION=0.2.1-beta`；`start-server.sh --version` 输出 `0.2.1-beta`；`--check-deps` 通过；6 个顶层文件 + `licenses/`。 |
| Linux tar 权限 | `start-server.sh`／`zt-installer.sh` 为 755，JAR／unit／README／VERSION／许可证为 644；不再受 WSL drvfs 的 chmod 限制。 |
| 客户端 SDK | `build/dist/ZTGUI-api-v0.5-alpha` 安装成功；`ZTGUIPluginApiConfig.cmake` 的 `ZTGUI_API_VERSION=0.5-alpha`；外部 SDK consumer 配置、编译、运行通过。 |
| 服务端 SDK | `ZTServer-api-v0.2-beta.zip`；sample 在本地 Maven 仓库独立编译通过。 |
| 归档 C++ 服务端 | `zt-server 0.4.1`、`ApiVersion=0.4`；WSL CTest 2/2 通过（`server_package_container`、`server_queries`）。 |

## 明确未验证

- macOS 实机、签名和公证；媒体／语音的 macOS 交付仍未实现。
- 真实多人麦克风／耳机听感、跨地域语音质量。
- 生产服务器部署、安装／卸载真实 ZeroTier 服务及真实网络中转联调。
- 插件热加载在用户真实运行目录中的长期行为（测试使用隔离临时目录）。

## 备注

- 客户端 EXE 的 Windows 字符串版本显示 `0.5.1-alpha`，数字版本字段为 `0.5.1`；关于界面显示客户端完整版本、客户端 API 和目标服务端版本 `0.2.1-beta`。
- 媒体 API、语音客户端和 Java 语音插件均针对新 API 重建；旧 0.4 API 包不能与新宿主混用。
- CMake 包版本使用数字基版本（客户端 0.5、服务端 0.2）；插件元数据使用完整 API 字符串（`0.5-alpha`／`0.2-beta`）。
