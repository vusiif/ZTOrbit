# 客户端 0.5.1／服务端 0.2.1 构建与验证

日期：2026-10-21。开发者指定客户端 `0.5.1`、服务端 `0.2.1`，分别去除原预发布后缀；同端 API 派生为 `0.5`／`0.2`，数字代际不变。历史验证保留原版本，不改写为本轮结果。

## 版本与构建

- 根 `VERSION=0.5.1`，`SERVER_VERSION=0.2.1`；客户端 EXE 的 FileVersion／ProductVersion 均为 `0.5.1`，Linux `--version` 返回 `0.2.1`。
- 客户端 SDK 的 `ApiVersion.h` 与 CMake 配置均为 `0.5`；服务端 API JAR 资源及 SDK 元数据均为 `0.2`。旧 `0.5-alpha`／`0.2-beta` 插件不再精确匹配。
- Windows 11 x64、MSVC 2022、动态 Qt 6.8.3；Linux 使用 WSL Ubuntu、现有 JDK 25 和 Maven 3.9.9。
- 媒体 `2.0.0`、语音客户端 `0.3.0`、Java 语音插件 `0.1.0` 的自身版本不变，均以本轮 SDK 重建。媒体复用受控 FFmpeg SDK，没有重新下载或编译 FFmpeg。

主要构建入口：

```powershell
$qtSdk = 'build/cmake-build-tools/Qt/6.8.3/msvc2022_64'
& ./.agents/skills/ztgui-build/scripts/build-windows.ps1 -QtRoot $qtSdk -SodiumRoot build/cmake-build-tools/third-party/libsodium/libsodium
& ./.agents/skills/ztgui-build/scripts/build-media-windows.ps1 -QtRoot $qtSdk -ApiSdk build/dist/ZTGUI-api-v0.5 -FfmpegRoot build/cmake-build-media-dependencies/installed
& ./.agents/skills/ztgui-build/scripts/build-plugin-windows.ps1 -PluginDirectory src/plugins/voice-chat/client -Target ztgui_voice_chat -QtRoot $qtSdk -ApiSdk build/dist/ZTGUI-api-v0.5 -AdditionalSdk build/dist/ZTGUI-media-api-v2
```

```bash
bash .agents/skills/ztgui-build/scripts/build-linux.sh --build-voice-plugin
```

客户端 SDK 消费者使用独立源码副本与新安装 SDK 构建；媒体预览消费者从已安装媒体 SDK 的 examples/preview 独立构建。构建、完整中间日志与临时检查位于忽略目录 `build/release-0.5.1-0.2.1/`，归档的关键输出位于[本轮证据目录](release-0.5.1-0.2.1/)。

## 本轮结果

| 检查 | 结果 |
| --- | --- |
| Windows 主程序与 SDK | 正式构建、部署、打包成功；独立 SDK consumer 编译运行成功，SDK sample 插件的包 API 为 `0.5` |
| Windows ZIP | 187 个文件的路径、逐文件内容和 CRC 均与运行目录一致；包内没有媒体实现库、业务插件或用户数据 |
| 媒体／语音包 | 两包 API 均为 `0.5`；媒体提供 session／clip 服务，语音声明媒体版本范围和双服务依赖；包入口与本轮二进制哈希一致 |
| 媒体实际加载 | `syntheticRoundtripAndIsolation`、`closeInsideFrameCallback` 通过；QtTest 合计 4 passed，含 init／cleanup，0 failed／skipped |
| 语音与预览实际加载 | 包内 Qt／CRT 核验、语音页面连接与卸载、独立预览像素检查通过；QtTest 合计 5 passed，含 init／cleanup，0 failed／skipped；使用 Windows 原生平台和默认 software 绘制 |
| Linux Maven | API 8 项、宿主 41 项、Java 语音 4 项，共 53 项通过 |
| Linux 启动／安装脚本 | StartupTests 9 项、完全模拟沙箱中的 InstallerTests 31 项通过；安装脚本测试以 WSL root 运行，没有安装真实服务或容器 |
| Linux 隔离集成 | 正式启动脚本＋宿主 6 项、正式启动脚本＋语音 6 项通过；与上述测试合计 105 项，0 failed／skipped |
| Linux 正式包 | `--check-deps`、版本、解压后监听、Java 插件加载／hello 和 SIGTERM 退出 0 通过；tar 脚本权限 0755、JAR 0644 |
| Linux SDK | ZIP CRC、元数据和 JAR 一致；SDK sample 在独立 Maven 仓库离线构建成功 |
| 文档与提交范围 | 当前版本及示例同步；原有 34 份待提交 Markdown 的 214 个本地链接无断链；根开发目标与步骤文件保留本地，不进入提交 |

## 修正与边界

- StartupTests 首次缺 Java 用例因 PATH 仍能找到系统 Java 而失败。仅修正测试隔离：缺 Java 用例 PATH 限定 fake bin，并为隔离环境补充 cat；重跑 9／9 通过。初次非 root 安装脚本测试被权限前置检查拒绝，确认完全模拟系统命令后以 WSL root 重跑 31／31 通过。
- 独立 ZIP 检查按发行包顶层目录解算路径后逐项比较；临时包检查中 socket 时限的假设已修正，没有因此修改产品源码。
- 本轮没有重跑完整 Windows GUI 回归、物理屏幕／音频长稳或多人语音听感，没有远端部署、真实 ZeroTier 安装／卸载或 macOS 验证。旧测试证据不等同于本轮结果。
- 既有 Maven 弃用／版本表达式提示及隔离测试的 Python socket ResourceWarning 没有造成测试失败；不以这些提示声称新增功能已经验证。
- Windows 主程序、媒体 SDK 和许可证目录由构建脚本产生的 `.previous-*`，以及已从仓库根移入 `build/` 的重复 FFmpeg SDK，因自动审批检查以 `blocked by policy` 拒绝删除而保留。备份均处于忽略目录，未纳入提交；未通过其他工具绕过删除限制。
- 历史及本轮原始测试日志中的空白行保持原样；源码、维护文档与新增验证说明单独执行空白检查，不改写原始输出。

11 个正式产物的文件大小和 SHA-256 见[产物记录](release-0.5.1-0.2.1-artifacts.json)。主程序、SDK、插件分别交付于 `build/dist/`，发行包不进入 Git。
