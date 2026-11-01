# 桌面共享测试

本目录覆盖 `desktop-share` 的共享协议、媒体本地回环与真实宿主插件面板。插件功能、使用条件和当前遗留问题见[客户端插件文档](../../plugins/desktop-share/client/docs/desktop-share.md)与[服务端插件文档](../../plugins/desktop-share/server/docs/desktop-share.md)。

## C++ 协议测试

`ProtocolGoldenTests.cpp` 与 `src/plugins/desktop-share/shared` 一起构建，读取双端共享金样例并逐字节验证 ZTS1 外层、控制 JSON 和 ZDS1 媒体头：

```powershell
cmake -S src/tests/desktop-share -B build/cmake-build-zds-protocol `
    -DCMAKE_PREFIX_PATH="<Z2-api-v0.5-261030>;<Qt 6.8 SDK>;<Z2-media-api-v2>"
cmake --build build/cmake-build-zds-protocol --config Release
ctest --test-dir build/cmake-build-zds-protocol -C Release --output-on-failure
```

## 真实宿主插件面板

`DesktopSharePluginTests.cpp` 需要真实宿主加载器、独立构建的客户端插件和媒体包，由根 `src/tests/CMakeLists.txt` 在设置 `ZTO_DESKTOP_SHARE_PLUGIN_PATH` 与 `ZTO_MEDIA_PLUGIN_PATH` 时加入。构建与运行：

```powershell
cmake -S . -B build/cmake-build-tests-host -G 'Visual Studio 17 2022' -A x64 `
    "-DCMAKE_PREFIX_PATH=<Qt 6.8 SDK>;<Z2-media-api-v2>" `
    -DZTO_BUILD_TESTS=ON `
    -DZTO_MEDIA_PLUGIN_PATH=build/dist/plugins/z2_media_worker-v2.0.0.z2plugin `
    -DZTO_DESKTOP_SHARE_PLUGIN_PATH=build/cmake-build-plugin-desktop-share/Release/z2_desktop_share.z2plugin
cmake --build build/cmake-build-tests-host --config Release --target desktop_share_plugin_tests --parallel

# 不启用真实设备：只验证页面加载、来源枚举不采集、缺失来源和卸载
./.agents/skills/zto-build/scripts/run-media-tests.ps1 `
    -QtRoot <Qt SDK> `
    -TestExecutable build/cmake-build-tests-host/Release/desktop_share_plugin_tests.exe `
    -HostRuntime build/dist/Z2-windows-x64 -TestArguments @('-platform','windows')

# 启用两模式×两档真实采集；结果及跳过项以运行日志为准
$env:ZTO_TEST_PHYSICAL_VIDEO='1'

# GPU Quick 后端覆盖：默认软件渲染上再加跑一遍；测试会自行关闭 QT_QUICK_BACKEND
$env:ZTO_TEST_GPU='1'
```

真实采集用例使用专用的动画窗口（320×240、360×240、500×420、720×300），保存的预览截图只包含该合成窗口；测试不保存用户桌面内容。等待用例会停止动画、检查 `waiting` 状态、最小化后恢复，并确认 `live` 恢复。

H.264 1080p 用例要求 25 秒内至少五帧，再观察两秒持续增长，每行同页面两轮启停。`staticWindowStartsWithoutRepaint` 属于常规物理视频套件，不使用已知失败开关跳过；它覆盖两模式／两档，每组同页两轮，保留五秒首帧断言，检查来源绘制次数不变、颜色、比例、黑边、停止后的计数和 12 秒静态保留。另一个等待初始来源变化的用例保留为缓存路径检查，不能替代无重绘冷启动。

`unloadWhileStaticCaptureIsActive` 对分块／H.264 两种模式在第二轮仍采集时直接卸载业务插件，并等待页面销毁和全部拥有的服务 worker 退出；它与先停止再卸载的常规路径分别覆盖。`desktop_share_udp_tests` 使用真实 localhost UDP socket 验证倒序／重复分片、丢片到期、SDK 新基准请求和发送轨道重启，并检查八包／8 MiB／200ms 重组边界。

## 回归与网络测试

`Stage4RegressionTests.cpp` 覆盖远端转发与控制回环、异步停止超时／重试、真实自定义端口连接与编辑／重连、停止后的旧回调、控制／UDP 跨通道次序和旧媒体异步结果失效，以及缺插件宿主、未就绪响应、TCP 接受但不回 hello、旧 hello 迟到、无关 `builtin` 消息和待创建动作清理。独立 CMake 需要已安装的宿主 SDK、媒体 SDK（Quick）和 Qt SDK；这组测试需要隔离的真实 Java 宿主监听 **127.0.0.1:18140**，缺插件用例还需要一个未安装插件的隔离宿主监听 **127.0.0.1:18142**，不能用生产服务替代。配置时的 `CMAKE_PREFIX_PATH` 同时包含三个 SDK 路径。

真实客户端慢观看者：`run-stage4-e2e.ps1 -Scenario single -Codec h264 -Profile 720p30 -SlowViewerMs 1500 -SlowViewerAtMs 4000 -SampleMs 400 ...` 先让第一名观看者稳定显示，再暂停第二名观看者的 GUI 1.5 秒；要求第二名触发邮箱丢包并恢复，第一名在同期 `frameSamples` 中持续增长。

真实服务端出口中断：`run-stage5-congestion.ps1 -Scenario egress-block` 用隔离 Java 宿主和 `loss_proxy.py` 把服务端→观看端 UDP 整体切断 2.5 秒后再恢复；观看端 `frameSamples` 必须出现与中断等长的帧平台，随后从新关键帧继续，`gapsDetected` 大于零，代理统计 `blockedServerUdp` 大于零。脚本同时验证慢观看者场景并写出 `stage5-e2e-summary.json`；详细参数与端口见脚本头部。

`Stage5CongestionTests.cpp` 覆盖发布队列活动包计入、4 包／4 MiB／200ms 上限，发布字节节奏和 1 MiB 突发，观看端 8 包／8 MiB／200ms 重组（后续片段不延长期限），以及待提交包 8 包／8 MiB 上限。真实宿主转发丢片使用 `loss_proxy.py` 默认 `--drop-mode sequence`；出口中断使用 `--drop-mode none` 加 `--block-after-server-udp`／`--block-for-ms`，不把两种故障混为一次结果。

无媒体首帧超时：`run-stage4-no-media.ps1 -QtRoot <Qt SDK> -Port 18140 -OutDir build/followup-stage4/no-media` 启动只维持控制心跳与 UDP 注册的协议发布者，真实插件观看者必须在十秒首帧期限退出观看，控制连接保持。与断 TCP 的用例分开记录。

真实宿主转发丢片：`loss_proxy.py --listen-port 18141 --server-port 18140 --stats <输出JSON> --seconds 90` 为本机验证提供透明控制／UDP 代理，只映射握手 UDP 端口，并丢弃指定非关键包的首片。真实 E2E 连接 18141，仍由 18140 的 Java 宿主转发；要求明确丢片数、观看端缺口数均大于零，同时通过原显示断言。代理不产生视频、不保存凭据或负载；测试结束只停止该测试代理。

`DesktopSharePluginTests::failedNetworkStopRetainsPluginAndProviderUntilRetry` 在真实插件网络线程安排 5.5 秒工作，要求 `requestStop` 立即返回、五秒排空失败向宿主传播、页面／插件／提供者保留，重试后正常卸载。E2E 截图仅保存专用合成来源的预览区域，不包含房号页面。

- `StreamLogicTests.cpp`：有序提交、乱序保持、重复、缺首片／末片、整包丢失 200ms 判定、关键帧与 discontinuity 恢复、变化块缓冲、晚加入不等待序号 0、跨代次／房间／会话／token 拒绝、冲突元数据、1024 序号窗口、有界邮箱和重组头暴露。
- `Stage5FollowupNetworkTests.cpp`、`Stage5RemediationTests.cpp`、`Stage5RemediationNetworkTests.cpp`：发布队列与字节节奏、整包准入与退款、依赖链清理、重组与待提交上限、GUI 通知板，以及 H.264／变化块的恢复语义。
- `Stage6LifecycleTests.cpp`、`Stage6RemediationTests.cpp`：页面生命周期、显式操作与取消、停止／卸载顺序、旧回调隔离、偏好字段合并和故障注入。
- `TileUdpTests.cpp`：真实 localhost UDP 分片、重组、丢片恢复和 SDK 新基准请求。
- 传输层不单独引入 C++ 假服务端：网络路径由真实 Java 服务端上的协议探针和真实客户端端到端进程验证，避免把模拟器通过当成真实转发证据。
- `video_probe.py`：用合成 ZDS1 片段在真实 Java 服务端上验证逐片扇出、token／session 替换、ready／代次门禁、序号窗口、乱序／重复、双房间隔离和关房停止转发；不打印 token 或房号。
- `packet_summary.py`：Linux 服务端原始包摘要工具，只输出端点与字节计数，不保存负载；用于说明上行一份、服务端扇出和无客户端直连。
- `netem-stage4.sh`：仅对 UDP 源端口 18100 施加 5% 丢包、2% 重复、25% 乱序和 20ms 延迟的弱网脚本；100% 黑障因影响管理链路未执行。
- `run-stage4-e2e.ps1`：`-Scenario matrix|single|switch|tworoom` 启动真实发布端／观看端进程，连接真实 Java 服务端，保存每个进程的 JSON、日志和页面截图。默认矩阵覆盖两模式×两档、共享中切换来源／档位和第二个隔离房间。

## Java 服务端测试

服务端房间控制测试位于 `src/plugins/desktop-share/server/src/test/`，随插件 Maven 项目运行：

```bash
mvn -f src/plugins/desktop-share/server/pom.xml clean verify
```

## 真实宿主协议探针

`control_probe.py` 只使用公开 ZTS1／ZDS1 线协议，连接已运行的服务端，不依赖客户端插件或媒体能力。默认按顺序执行 7 个场景：

```bash
python3 src/tests/desktop-share/control_probe.py --host 127.0.0.1 --port 18100 --mode all
```

可用 `--mode flow` 等单独运行。重启失效校验使用两阶段模式：

```bash
# 阶段 1：创建并保持一个已配置房间，把房号写入文件
python3 src/tests/desktop-share/control_probe.py --mode hold-room --hold-seconds 120 --code-out /tmp/zds-code.txt
# 停止并重启服务端
# 阶段 2：旧房号应返回 ROOM_NOT_FOUND
python3 src/tests/desktop-share/control_probe.py --mode join-only --code "$(cat /tmp/zds-code.txt)"
```

探针不进入发行包，不把测试日志、截图或模拟客户端写入运行数据。`run-stage4-e2e.ps1`、`video_probe.py` 和 `packet_summary.py` 同样只用于验证，不进入发行包。
