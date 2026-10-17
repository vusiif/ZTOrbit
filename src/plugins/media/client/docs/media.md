# 通用媒体服务与双 SDK

媒体提供者 `media 2.0.0` 在独立 worker 中提供 `media.session / 1`。宿主（客户端）版本为根 VERSION 的 `0.5.1-alpha`，宿主 SDK 为 `0.5-alpha`。本代删除旧音频服务和 AudioClient，不提供兼容入口。

## 分层与会话

业务插件拥有房间、成员、网络协议和界面。媒体引擎只认识来源、轨道、格式、时间戳和处理链；不得让宿主依赖媒体实现。

每个服务租约拥有一个 MediaSession。会话可创建音视频发送／接收轨道，通过停止后重新配置更新格式；启动结果表示设备绑定和处理管线已接受启动，设备随后发生的错误通过事件报告。查询来源不会打开采集。设备消失或授权失败不自动恢复采集。

设备与录放音由专属事件线程管理，每个会话的编解码和扩展处理运行在自己的执行线程。输入／输出排队均有界，兼容来源可复用采集，输出按设备混音，音量与流归消费者所有。不兼容的视频采集请求返回冲突，不静默修改其他会话。

## Media API SDK

`ZTGUIMediaApi 2.0.0` 提供三个目标：

- `ZTGUI::MediaTypes`：仅 Qt Core，包含来源、格式、编解码配置、轨道、编码包、FrameView 和结果类型。
- `ZTGUI::MediaApi`：Qt Core/Network 和宿主 `ZTGUI::Services`，包含异步 MediaSession 客户端，不传递 Quick/QML 或 Multimedia 链接依赖。
- `ZTGUI::MediaQuick`：可选静态帧呈现组件，使用 `find_package(ZTGUIMediaApi 2 CONFIG REQUIRED COMPONENTS Quick)` 获取。组件由页面工厂创建，无全局 QML 注册。

宿主 SDK 的 `Services` 组件只要求 Core/Network。普通界面插件仍显式链接 `ZTGUI::Api`。

客户端通过声明过的提供者获取 `media.session / 1` 租约后构造 MediaSession。先 `open()` 建立数据通道，再创建并启动轨道。控制 API 包含 capabilities、sources、createTrack、configure、start、stop、remove、requestKeyFrame、setGain、statistics、close；submit 提交外部编码包，submitTo 根据接收轨道描述自动填写本地路由与配置代次，packet/frame/error/sourcesChanged 为事件回调。

跨进程仅使用 DTO 和带代次的 ID。EncodedPacket 携带轨道、配置代次、编码格式、初始化数据、序号、PTS、可选 DTS、时间基、时长、关键帧及不连续标记。默认时间单位为微秒；共同 syncGroup 将远端轨道时间映射到同一播放时钟。

公开视频预览输出为全范围 BGRA，最大 1920×1080；音频基线为单声道 48 kHz f32、Opus 20 ms。`CodecConfig` 的 `bitrate` 默认值 24000 面向 Opus；视频轨道必须显式设置码率，有效范围 64000–20000000（预览示例使用 2 Mbps）。视频默认分辨率／帧率为 1280×720、30 fps、无 B 帧，最长一秒关键帧间隔。能力查询区分编译能力、本机可用能力及缓冲限制。硬件编码暂未启用，显式请求会返回不支持。

## 数据与生命周期

控制 IPC 继续由宿主管理。通用 `$channel` 协商每租约独立的本机数据连接，使用随机凭据、租约和提供者代次认证；宿主不解析媒体数据。单条数据上限 8 MiB，以 64 KiB 分块，待写上限 16 MiB，接收只重组一条有界消息。

预览使用 Windows 三槽共享内存，槽位有空闲、写入、就绪和读取状态。FrameView 只在回调期间有效，保留数据必须复制。MediaQuick 复制最新图像后立即归还槽位。消费者处理慢时采集邮箱保留最新帧，预览替换尚未读取的旧帧；槽位版本校验防止旧通知读取新内容，正在读取的帧不会被覆盖；编码队列溢出报告不连续并请求新关键帧。

stop 停止轨道并清空缓冲，保留配置；remove 销毁轨道；close 禁止新调用并关闭数据通道，等待设备解绑和会话执行线程排空后释放租约。直接数据回调持有宿主 callbackGuard，防止嵌套事件循环在回调返回前卸载消费者 DLL。提供者异常退出使旧代次失效，不自动恢复录音。

## Engine SDK 与扩展

`ZTGUIMediaEngine 1.0.0` 导出 `ZTGUI::MediaEngine`，携带完整受控 FFmpeg 头文件、导入库、运行库和实际构建记录。扩展仅在 worker 内使用 AVFrame 等 FFmpeg 对象；这些类型不进入 Media API。

引擎依赖 FFmpeg 7.1.5、OpenH264 2.6.0 和 Opus 1.6.1。FFmpeg 源码构建启用 avcodec、avformat、avutil、avfilter、avdevice、swresample、swscale，H.264 软件编码使用 libopenh264。音频编解码／PLC 使用独立 Opus 后端，PCM 转换和重采样使用 swresample。

扩展导出 `ztmedia_extension_v1`，描述 C ABI 结构大小、ABI 版本、FFmpeg 二进制指纹和处理器工厂。每条处理链独立创建实例，process/drain 在会话线程同步执行；输入仅在调用期间有效，输出回调期间有效，引擎通过 av_frame_clone 取得需要的引用。Engine ABI 1 每个输入最多输出一帧，保持协商后的格式；音频保持 960 样本块。多输出处理器明确报错，不会静默丢弃输出。AVFrame 的 time_base 描述处理节点输入的时间单位。扩展不得留下未排空线程或异步回调，也不得让异常穿过 C ABI。

扩展 DLL 与 extensions.json 随认证媒体包交付，只按清单加载；扩展使用同一组 FFmpeg DLL，不携带另一份运行库。入口、指纹或 ABI 错误使服务准备失败。模块保留到 worker 退出，实例随轨道释放；不支持运行中热替换，也不提供扩展间安全隔离。

示例 `examples/flip` 使用 libavfilter hflip。引擎 SDK 安装后可在仓库外独立构建它。SDK 的 ztmedia_write_extension_bundle 生成扩展打包清单；示例构建后在 Release/extension-bundle.json 输出该文件。将它传给 build-media-windows.ps1 的 -ExtensionBundle，或 CMake 的 MEDIA_EXTENSION_BUNDLE，即可替换 worker 的扩展集合，无需修改服务源码。清单为 source 与 path 对象数组，source 可相对清单目录，path 必须位于 extensions/；服务启动仍验证 DLL 内的真实 ABI 和构建指纹。

## 构建、预览和测试

从仓库根调用 `.agents/skills/ztgui-build/scripts/build-media-windows.ps1 -QtRoot <Qt SDK> -ApiSdk <宿主 SDK>`。首次调用通过 build-ffmpeg-windows.ps1 构建受控依赖；可用 `-FfmpegRoot` 指向已验证依赖 SDK，`-OpusSource` 指向离线 Opus 源码。工具使用固定源码 URL 和 SHA-256；不修改系统 PATH 或 Qt Kit。

交付目录为 `build/dist/ZTGUI-media-api-v2/`、`build/dist/ZTGUI-media-engine-sdk-v1/` 及各自 ZIP，媒体包为 `build/dist/plugins/ztgui_media_worker-v2.0.0.ztplugin`。媒体运行库、Qt 媒体后端和 CRT 只进入 worker 包，收集器显式使用同一组受控 FFmpeg 替换 Qt Kit 的同名库，并校验实际二进制指纹与许可证来源。

API SDK 的 `examples/preview` 是仅用于开发的预览插件，可显式选择来源，展示采集和 H.264 回送图像；不进入主程序发行包。它只使用公开宿主／媒体 SDK。

算法与通道测试位于仓库 `src/tests/media/`，媒体项目用 `MEDIA_BUILD_TESTS=ON` 配置；宿主集成测试用 `ZEROTIER_BUILD_TESTS=ON`、`ZTGUI_MEDIA_PLUGIN_PATH=<媒体包>` 配置。分别设置 `ZTGUI_TEST_PHYSICAL_VIDEO=1`、`ZTGUI_TEST_PHYSICAL_AUDIO=1` 启用实际设备；`ZTGUI_MEDIA_SOAK_SECONDS=600` 启用十分钟测试。真实设备不可用会明确跳过，不等同于验收通过。

使用构建技能中的 run-media-tests.ps1 指定 QtRoot、TestExecutable、SupportBin 和 TestArguments；实际宿主插件加载测试还传 -HostRuntime build/dist/windows-x64 准备匹配宿主的应用目录基础 DLL。启动器检查 qwindows.dll 及本地 Qt Core 一致性，设置正确插件路径，并对无人值守测试设置退出时限。worker 自己的插件路径独立于测试进程，不继承父进程的 QPA 路径或 headless 平台选择。

本代仅验证 Windows x64。系统音频回环、GPU 共享、完整视频业务插件、任意处理图编辑器和其他平台交付不在范围内。
