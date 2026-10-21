# 2026-10-15 插件服务迁移验证

本轮范围为 Windows x64 的通用服务框架、独立媒体与现有语音迁移。应用／宿主 SDK 保持 0.4.1／0.4，入口及生命周期 /3.0；新增 ServiceAccess 和 PluginStopInterface/1.0，包格式 ZTPK3，媒体服务及媒体 SDK 1.0.0，语音 0.2.0。没有创建远端、修改历史、升级应用版本或部署生产服务。

本页下方的初次迁移结果及哈希是当时的快照。随后根据 Windows 11 23H2／管理员运行的失败反馈修正 worker 启动与部署；该次修正的证据和新产物以 [worker 启动修正](../plugin-worker-startup-2026-10-15/evidence.json) 为准。服务端后续部署由语音服务端项目文档记录。

后续23:09日志进一步确认了18秒设备枚举超过握手期限的直接原因；最新两阶段准备修正及产物见 [慢设备准备验证](../plugin-worker-preparation-2026-10-15/evidence.json)。此前部署隔离回归仍有效，但其快照不能代表这次日志的原因已在当时验证。

## 最终检查与证据

Windows 环境为 Windows 11 x64、MSVC 2022、动态 Qt 6.8.3 Release。以下为最终回归，计数采用 QtTest 输出（包括初始化和清理）；原始输出、认证清单、产物哈希和逐轮模块记录保存在 [证据目录](evidence.json)。

| 检查 | 结果 | 原始输出 |
| --- | --- | --- |
| 通用服务与依赖 | 19 passed，0 failed | [services](services.txt) |
| 旧插件加载器 | 17 passed，0 failed | [loader](loader.txt) |
| 客户端容器 | 10 passed，0 failed | [container](container.txt) |
| 核心行为 | 16 passed，0 failed | [core](core.txt) |
| 原生 Windows GUI | 27 passed，0 failed，1 skipped | [gui](gui.txt) |
| GPU 临时启动覆盖补测 | 3 passed，0 failed | [gpu-override](gpu-override.txt) |
| 实际媒体／语音包集成 | 5 passed，0 failed，2 skipped | [voice-integration](voice-integration.txt) |
| 媒体算法／编解码 | 6 passed，0 failed | [media-audio](media-audio.txt) |
| 语音协议、UI 与配置 | 8 passed，0 failed | [voice-ui](voice-ui.txt) |
| Ubuntu／Qt 6.4 旧服务端容器 | CTest 1/2（本次仅运行 `server_package_container`），QtTest 3 passed | [server-container](server-container.txt) |

GUI 组跳过的 GPU 参数用例已通过独立带参数运行补齐；语音组两项跳过需要 `ZTGUI_LIVE_VOICE_HOST` 的外部服务器用例。曾在负载下出现既有网络轮询用例的 5 秒等待不足；业务离线判定本身至少等待 5 秒，现将测试等待设为 8 秒，单项复测及最终完整 GUI 均通过，生产判定逻辑未改动。

## 对照实施计划

| 要求／场景 | 实现与直接证据 |
| --- | --- |
| 契约与 ABI | 新增独立 ServiceAccess、ServiceLease 和 PluginStopInterface；原 HostServices 虚表、PluginDefinition 布局及 /3.0 IID 保持。安装后 SDK 包含公开接口与通用 wire／worker 工具。 |
| 执行前依赖校验 | services 覆盖严格版本／服务、自依赖与传递环、缺失／禁用／规范化重名；无消费者入口 trace。认证完整目录、保存事务包快照，提取时核对包一致性。 |
| 新格式与旧包 | container、loader 及 Linux 容器覆盖 v1/v2、篡改、路径／链接、截断、尾随数据、API/IID 拒绝；services 验证 v3 依赖认证、禁止降级及能力握手。旧 SDK 工具拒绝 v3，见 [旧读取器输出](legacy-reader.txt)。 |
| 共享提供者与独立会话 | services 同时加载 fixture.audio、fixture.video 两个消费者，只有一个提供者 PID；卸载一个后另一个实际二进制 echo 会话继续成功。夹具不代表屏幕共享业务。 |
| 加载、取消、回滚与竞争 | services 覆盖初始化失败、显式提供者保留、握手期取消、身份未知时取消、自动停止中再加载及新代次；loader 覆盖大包心跳、重复请求合并、后台初始化取消。 |
| 逆序停止与失败 | services 验证消费者停止失败时保留自身与提供者、重试后释放；loader 验证销毁失败不报 shutdown 完成、页面／工厂清理及嵌套回调。服务拒绝回调即使绑定宿主 context，仍在返回前保留 DLL，close 会取消排队回调。 |
| 异常与进程树所有权 | services 验证服务崩溃使旧租约失效、新实例代次不同；停止超时强制结束专属 Job，worker 和实际子进程句柄均确认已退出；另杀死隔离测试宿主，核对整棵拥有的进程树退出。 |
| 原生控件与退出 | gui 深浅两组通过实际点击查看服务、右键卸载、取消／确认级联并核对结果；退出和初始化期间保持心跳。截图位于本地 .tmp/screenshots/plugin-services-*.png、plugin-cascade-*.png，已检查文字、层次和按钮。 |
| 独立项目与 SDK | 媒体、语音完整源码复制到仓库外；当前安装的宿主／媒体 SDK 与副本逐文件哈希一致，独立配置、编译和 package 目标成功。开启项目自带算法／协议／UI 测试后也可构建运行，生成项目没有宿主源码引用；源文件及 SDK 哈希见 evidence.json。 |
| 实际媒体实现 | media-audio 验证 PCM、独立增益、流式重采样、Opus、抖动缓冲；voice-ui 验证深浅主题、端点、音量、异步枚举前保存的设备偏好及停止后禁止新业务。真实设备结果见下节。 |
| 完全卸载与循环 | 实际包执行 2 轮暖机＋20 轮，各轮核对媒体清单导出的模块名称、实际路径和 SHA-256；结束后主进程无媒体／语音业务映射、worker 退出，runtime 锁句柄／目录占用为 0，GUI 心跳持续。 |
| 发行分离 | ZIP CRC 和逐文件哈希与发行目录一致；主程序 EXE 与 Release 构建相同，文件／产品版本 0.4.1。主程序无业务包、Multimedia、FFmpeg 和媒体后端；媒体 API 无 DLL，语音只有业务 DLL。 |

## 真实音频与资源记录

最终真实设备测试明确选择 `阵列麦克风 (AMD Audio Device)` 和 `扬声器 (Senary Audio)`；另枚举到网易虚拟音频设备。采集／播放均成功打开，观测 `capturedFrames=18`、`receivedFrames=17`、`playedBytes=130552`，经 Opus、独立控制／二进制 socket、解码和播放写入完成本机回送。输出增益为 0，音频不保存、不发送外部服务器；这证明设备 IO 路径，不代表多人听感评估。

最终 22 轮记录在 [module-observations.jsonl](module-observations.jsonl)：22 个不同代次，每轮拥有的进程已退出，宿主媒体映射为 0。宿主句柄范围 412–419，线程 9–15，无逐轮累积；具体数组见 evidence.json。实际 worker 映射包含 Qt6Multimedia、ffmpegmediaplugin 及 FFmpeg 各库，其哈希逐一匹配 [媒体认证清单](media-manifest.json)，不是只按固定 DLL 名称判断。

语音 DLL 普通及延迟 PE 导入已核对，与 [语音认证清单](voice-manifest.json) 的业务 DLL SHA-256 相同，仅导入宿主基础 Qt、CRT／系统库，无 Multimedia、FFmpeg、Opus 或媒体实现。媒体包共 43 个清单文件，携带自己的 Qt 基础库、后端、FFmpeg、MSVC 运行库和对应许可；语音包共 1 个文件。

## 构建入口与交付

主程序与两端 SDK 使用项目构建技能；媒体使用 `build-media-windows.ps1 -QtRoot <Kit> -ApiSdk build/dist/ZTGUI-api-v0.4 -OpusSource <1.6.1源码>`；语音使用 `build-plugin-windows.ps1 -PluginDirectory src/plugins/voice-chat/client -Target ztgui_voice_chat -QtRoot <Kit> -ApiSdk build/dist/ZTGUI-api-v0.4 -AdditionalSdk build/dist/ZTGUI-media-api-v1`（本轮媒体 API 为 1.0.0；媒体 2.0 起使用 v2 SDK）。脚本位置均在 `.agents/skills/ztgui-build/scripts/`。

已生成 `build/dist/windows-x64.zip`、`build/dist/ZTGUI-api-v0.4.zip`、`build/dist/ZTServer-api-v0.4.zip`、`build/dist/ZTGUI-media-api-v1.zip`，以及 `build/dist/plugins/ztgui_media_worker.ztplugin`、`ztgui_voice_chat.ztplugin` 和对应许可证目录。四个 ZIP 分别含 191／47／29／5 个文件；大小和完整 SHA-256 见 [产物审计](artifacts.json)。旧输出保留在 previous 备份中。

本机复现测试时，将 `cmake-build-tools/Qt/6.8.3/msvc2022_64/bin` 加入当前进程 PATH，QT_PLUGIN_PATH 指向该 Kit 的 plugins；GUI 使用 `-platform windows`。主程序打包使用 `build-windows.ps1 -QtRoot <Kit> -SodiumRoot .tmp/relay-deps/sodium/libsodium`；两端 SDK 因公开接口／共享打包输入变化而更新，未更改 VERSION。服务端仅重新生成 SDK 和验证旧容器，不构建或部署服务端业务插件。

仓库外证明目录为 `C:/Users/27384/AppData/Local/Temp/ztgui-sdk-proof-2eb732489ffd46629a5e89e98785c1fe`，包含 media、voice、host-sdk、media-sdk 和离线 Opus。媒体使用 `cmake -S <副本>/media -B <副本>/build-media -DCMAKE_PREFIX_PATH=<host-sdk>;<media-sdk>;<Kit> -DFETCHCONTENT_SOURCE_DIR_OPUS=<副本>/opus`，语音使用 `cmake -S <副本>/voice/client -B <副本>/build-voice -DCMAKE_PREFIX_PATH=<host-sdk>;<media-sdk>;<Kit>`（PowerShell 将整个 -D 参数加引号），然后分别构建 Release 的 `ztgui_media_worker_package`、`ztgui_voice_chat_package`。两者均成功；MSBuild 对临时目录的 MSB8029 是增量生成提示。

同一副本另开启 `-DMEDIA_BUILD_TESTS=ON`、`-DVOICE_BUILD_TESTS=ON`，构建并运行 `media_audio_tests`、`voice_ui_tests`，分别 6／8 项通过。语音测试源码位于 `src/tests/voice-chat/VoiceUiTests.cpp`，由语音项目在 `-DVOICE_BUILD_TESTS=ON` 时引用；旧的跨项目音频测试已拆为媒体算法与语音协议用例，不再保留重复且无构建入口的源码。

真实设备复现另设 `ZTGUI_TEST_PHYSICAL_AUDIO=1`、`ZTGUI_TEST_AUDIO_INPUT=AMD Audio Device`、`ZTGUI_TEST_AUDIO_OUTPUT=Senary Audio`，运行 `cmake-build-qml/Release/voice_plugin_tests.exe -platform windows`。该程序会执行实际设备回送、22 轮加载卸载和面板点击；未设置外部服务器时明确跳过两项 live 用例。GUI 使用 `zerotier_gui_tests.exe -platform windows`；GPU 补测增加 `--disable-gpu-acceleration gpuAccelerationCommandLineOverrideIsTemporary`。

## 明确边界

截至 2026-10-15，媒体仅验证 media.audio，未实现屏幕采集、视频传输和观看；媒体 2.0 的视频能力见[媒体文档](../../../src/plugins/media/client/docs/media.md)与[媒体验收](../media-engine-2026-10-17/README.md)。没有重新连接生产语音服务、多用户听感评估或生产部署。macOS/Linux 客户端、签名／平台媒体权限、GPU 跨进程共享不在已验证范围。独立源码暂存于现有仓库的 src/plugins 下；仓库外副本仅用于安装 SDK 的独立构建证明。
