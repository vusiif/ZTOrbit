# 通用媒体服务与双 SDK

媒体提供者 `media 2.0.0` 在独立 worker 中提供 `media.session / 1`。宿主（客户端）版本为根 VERSION 的 `0.5.2`，宿主 SDK 为 `0.5`。本代删除旧音频服务和 AudioClient，不提供兼容入口。

## 分层与会话

业务插件拥有房间、成员、网络协议和界面。媒体引擎只认识来源、轨道、格式、时间戳和处理链；不得让宿主依赖媒体实现。

每个服务租约拥有一个 MediaSession。会话可创建音视频发送／接收轨道，通过停止后重新配置更新格式；启动结果表示设备绑定和处理管线已接受启动，设备随后发生的错误通过事件报告。查询来源不会打开采集。设备消失或授权失败不自动恢复采集。

设备与录放音由专属事件线程管理，每个会话的编解码和扩展处理运行在自己的执行线程。输入／输出排队均有界，兼容来源可复用采集，输出按设备混音，音量与流归消费者所有。视频发送轨道在编码前统一合成固定尺寸的 BGRA 画布：画布宽高来自轨道配置（会话校验要求为偶数），来源按原始宽高比缩放并居中，多余区域填黑；编码和预览使用同一画布，不把 4:3、16:10、超宽屏或竖屏来源拉伸到目标比例。来源尺寸与画布一致时只做像素格式转换，不重采样。

来源列表覆盖麦克风、扬声器、摄像头、屏幕、窗口和测试图案。屏幕增删会发出 `sourcesChanged`；活动屏幕来源的显示器被移除时，该采集以设备错误结束。Windows 窗口由 worker 内的 Windows Graphics Capture 采集，使用捕获项尺寸建池并保留首个真实帧，解决无后续重绘窗口的冷启动。最小化暂停取帧；捕获项关闭或原生窗口失效立即报告来源错误。每秒枚举仍用于来源更新与持续不可用探测，单次缺失不停止；其他平台保留 Qt 窗口路径及十秒探测，`QWindowCapture::NotFound` 不立即失败。不兼容的视频采集请求返回冲突，不静默修改其他会话。实现及边界见 [Windows 窗口采集](window-capture.md)。

## Media API SDK

`ZTOrbitMediaApi 2.0.0` 提供三个目标：

- `ZTO::MediaTypes`：仅 Qt Core，包含来源、格式、编解码配置、轨道、编码包、FrameView 和结果类型。
- `ZTO::MediaApi`：Qt Core/Network 和宿主 `ZTO::Services`，包含异步 MediaSession 客户端，不传递 Quick/QML 或 Multimedia 链接依赖。
- `ZTO::MediaQuick`：可选静态帧呈现组件，使用 `find_package(ZTOrbitMediaApi 2 CONFIG REQUIRED COMPONENTS Quick)` 获取。组件由页面工厂创建，无全局 QML 注册。

宿主 SDK 的 `Services` 组件只要求 Core/Network。普通界面插件仍显式链接 `ZTO::Api`。

客户端通过声明过的提供者获取 `media.session / 1` 租约后构造 MediaSession。先 `open()` 建立数据通道，再创建并启动轨道。控制 API 包含 capabilities、sources、createTrack、configure、start、stop、remove、requestKeyFrame、setGain、statistics、close；submit 提交外部编码包，submitTo 根据接收轨道描述自动填写本地路由与配置代次，packet/frame/error/sourcesChanged 为事件回调。

`statistics().values.tracks` 提供每轨累计计数。除布尔状态和当前队列长度外，64 bit 计数／微秒时间使用十进制字符串。视频诊断字段不改变公开 API 签名或 ABI：

| 字段 | 含义 |
| --- | --- |
| `captured` / `received` / `dropped` | 进入发送处理的采集帧、接收编码包、既有混合丢弃计数；`dropped` 不能单独用于定位 |
| `decodedOutput` / `decodedQueueDrops` | 解码器输出帧数／三帧待显示队列溢出淘汰数 |
| `decodedQueueSize` / `decodedQueuePeak` | 当前待显示队列长度／累计峰值 |
| `playbackReanchors` | 独立 H.264 接收轨道因三帧队列全在未来、提前量超出可容纳窗口而重建播放锚点的次数 |
| `firstDecodedPts` / `lastDecodedPts` / `lastPacketPts` | 首／最近解码输出的微秒 PTS，以及最近输入包的原时间基 PTS |
| `scheduleAheadUs` / `peakScheduleAheadUs` / `frontAheadUs` | 最近帧调度时的提前量／最大正提前量／查询时队首距显示时刻的差值；空队列的队首差值为 0 |
| `tickCount` / `tickDrained` / `peakTickGapUs` | 活跃期间定时器执行次数／从待显示队列取出的帧数／最大定时器间隔 |
| `frames` / `previewPublished` | 预览发布尝试数／预览头成功进入 actor 输出队列的次数；二者均不保证消费者已取得或显示图像 |
| `previewPublishFailures` / `previewOverwritten` / `previewDataFailures` | 共享槽发布失败／替换未读槽／预览头被 actor 输出队列拒绝的次数 |
| `processingUs` / `peakProcessingUs` | 发送画布合成、预览和编码的累计／最大耗时 |
| `decodeUs` / `peakDecodeUs` / `previewUs` / `peakPreviewUs` | 解码（含格式转换）和共享槽发布的累计／最大耗时 |
| `keyFrameRefreshes` / `videoBeforeStart` | 用缓存画布响应关键帧请求的次数／到达尚未运行轨道的有效视频输入次数；刷新不计入新采集 |
| `actorVideoFrames` / `invalidVideoFrames` / `startupFramesUsed` | 到达 actor 的输入／无效输入／在 running 提交时消费的暂存首帧；暂存重放不重复增加 actor 输入数 |
| `mailboxOffered` / `mailboxDelivered` / `mailboxReplaced` / `mailboxQueueDrops` / `mailboxCancelled` | 当前启动代次的设备回调输入、投递到 actor、合并替换、队列拒绝和取消计数 |

这些计数只包含状态和时间，不包含图像内容。成功显示需另由消费者回调和页面验证，不能把 worker 接纳发送当作消费成功。

`statistics().values.devices.rawVideoFrames`／`cpuVideoFrames` 是设备线程累计收到的视频输入／转换为有效 CPU 帧的次数，不是单轨采集计数；可与轨道 `captured` 对照定位采集、转换和投递环节。`devices.sources` 按 `tracks` 关联当前来源，提供对应计数、`captureActive`、`consumerDeliveries` 与 `cachedDeliveries`。Windows 来源在专用 MTA 线程转换，另有 `native.notifications`、`nativeFrames`、`cpuFrames`、`apartmentType`（MTA=1）、`poolRecreates`、初始尺寸，以及单槽回送的 `outputOffered`／`outputDelivered`／`outputReplaced`／`outputPending`。原生通知、回送和 Qt sink 通知不是同一层，不能只凭 captured=0 判定设备没有交付。64 bit 计数使用十进制字符串。

跨进程仅使用 DTO 和带代次的 ID。EncodedPacket 携带轨道、配置代次、编码格式、初始化数据、序号、PTS、可选 DTS、时间基、时长、关键帧及不连续标记。默认时间单位为微秒；共同 syncGroup 将远端轨道时间映射到同一播放时钟。

公开视频预览输出为全范围 BGRA，最大 1920×1080；音频基线为单声道 48 kHz f32、Opus 20 ms。`CodecConfig` 的 `bitrate` 默认值 24000 面向 Opus；视频轨道必须显式设置码率，有效范围 64000–20000000（预览示例使用 2 Mbps）。视频默认分辨率／帧率为 1280×720、30 fps、无 B 帧，最长一秒关键帧间隔。能力查询区分编译能力、本机可用能力及缓冲限制。硬件编码暂未启用，显式请求会返回不支持。

视频另提供面向文字桌面的 `bgra-tiles`：变化块无损编码、带宽预算下轮换发送、完整基准及丢包恢复。使用方式、负载、资源边界和 SDK 迁移见[自适应分块](tiles.md)。H.264 保留用于持续运动画面。`keyFrameRequired` 回调由应用映射到发送者；`Statistics::track(id)` 可直接读取轨道快照。

H.264 单个 IDR 可立即解码，无需等待下一编码包。编码包保留实际输入的微秒时间戳，避免低帧率下强制关键帧被内部整数帧刻度推到未来；处理器相对于输入的时间偏移仍保留。已有画布时 `requestKeyFrame()` 可刷新静态来源；尚未收到真实采集帧时不生成替代画面。共享来源保留最近一个 CPU 帧给新消费者，缓存种子按本次重放时间戳记入独立交付计数，不能用旧来源 PTS 偏置新轨道播放锚点；首帧早于 running 到达时，actor 有界暂存，在启动提交时处理，停止即释放。Windows 无重绘冷启动已补充原生采集回归；启动成功本身仍不表示已显示画面。

视频编码限帧使用独立的预算补充游标与最后编码时间；拒绝输入也推进补充游标，每段时间只补充一次。预算以整数微秒累计，容量仍为两帧；启动时允许最多两帧突发，任意前缀获准数不超过 `2 + floor(配置帧率 × 已过微秒 / 1,000,000)`，长停顿也最多存两帧。相同或倒退的已知时间戳不补充、不消耗预算、不回退游标，必须等时间戳超过高水位或显式停止／重新启动；未知采集时间使用当前单调时钟。停止清除预算与时间戳，重新启动建立新生命周期，运行中重复 start 不重新发放预算。合成来源推进自己的计时截止时间，并与真实采集、缓存关键帧刷新共用同一编码预算；关键帧请求不会额外增加长期编码速率。

不带 `syncGroup` 的 H.264 实时接收轨道保持三帧待显示队列。当队列已满、队首尚在未来且最新帧提前量超过 60ms 目标延迟加三个配置帧间隔时，丢弃待显示旧帧并按最新输入重建同一轨道锚点，避免旧首包或初始化耗时导致队列永久饿死。该有界恢复不调整共享音视频组的时钟，不改变预览共享槽或增加缓存。

## 通用短音服务 media.clip / 1

媒体 worker 同时提供 `media.clip / 1`，公开客户端为 `ztmedia/AudioClipClient.h`。它只播放短提示音，不含房间、成员或语音概念；与 `media.session` 共享输出设备和混音器，但独立租约。

`play(device, gain, wav, ...)` 通过控制调用发送 0–2 增益和输出设备，WAV 走二进制 payload；成功只表示请求已接受并返回 clip ID，实际完成通过 `finished`／`failed` 事件报告。每个 clip 恰好产生一个终态事件：成功为 `finished`；设备消失、某块被输出拒绝或混音队列未在限时内排空为 `failed`，不会静默改发 `finished` 或永久卡住。`stop` 和关闭租约是静默取消，不发送失败事件。只接受 RIFF/WAVE、PCM16、48 kHz、单声道、总 payload ≤256 KiB、时长 ≤2 秒，并校验 RIFF chunk 的边界、填充与块对齐。每个租约只允许一个活动 clip；`stop` 和关闭租约幂等。播放按 20 ms 分块投喂输出队列，每块检查设备是否接受；最后一块之后查询该轨混音队列和尚未写入输出的积压，二者都为空才报告完成并解绑，不用固定节拍近似尾音；解绑后的输出对象在有限时间内继续排空设备缓冲，不立即销毁。主动停止可丢弃未混音数据。媒体不负责排队，语音插件维护自己的有界队列。

同一媒体包和 manifest 同时声明两个服务；缺少 `media.clip` 的旧包会在消费者依赖规划阶段被拒绝。短音服务不打开麦克风、不创建发送轨道。

## 数据与生命周期

控制 IPC 继续由宿主管理。通用 `$channel` 协商每租约独立的本机数据连接，使用随机凭据、租约和提供者代次认证；宿主不解析媒体数据。单条数据上限 8 MiB，以 64 KiB 分块，待写上限 16 MiB，接收只重组一条有界消息。

预览使用 Windows 三槽共享内存，槽位有空闲、写入、就绪和读取状态。FrameView 只在回调期间有效，保留数据必须复制。MediaQuick 复制最新图像后立即归还槽位。消费者处理慢时采集邮箱保留最新帧，预览替换尚未读取的旧帧；槽位版本校验防止旧通知读取新内容，正在读取的帧不会被覆盖；编码队列溢出报告不连续并请求新关键帧。

stop 停止轨道并清空缓冲，保留配置；remove 销毁轨道；close 禁止新调用并关闭数据通道，等待设备解绑和会话执行线程排空后释放租约。直接数据回调持有宿主 callbackGuard，防止嵌套事件循环在回调返回前卸载消费者 DLL。提供者异常退出使旧代次失效，不自动恢复录音。

## Engine SDK 与扩展

`ZTOrbitMediaEngine 1.0.0` 导出 `ZTO::MediaEngine`，携带完整受控 FFmpeg 头文件、导入库、运行库和实际构建记录。扩展仅在 worker 内使用 AVFrame 等 FFmpeg 对象；这些类型不进入 Media API。

引擎依赖 FFmpeg 7.1.5、OpenH264 2.6.0 和 Opus 1.6.1。FFmpeg 源码构建启用 avcodec、avformat、avutil、avfilter、avdevice、swresample、swscale，H.264 软件编码使用 libopenh264。音频编解码／PLC 使用独立 Opus 后端，PCM 转换和重采样使用 swresample。

扩展导出 `ztmedia_extension_v1`，描述 C ABI 结构大小、ABI 版本、FFmpeg 二进制指纹和处理器工厂。每条处理链独立创建实例，process/drain 在会话线程同步执行；输入仅在调用期间有效，输出回调期间有效，引擎通过 av_frame_clone 取得需要的引用。Engine ABI 1 每个输入最多输出一帧，保持协商后的格式；音频保持 960 样本块。多输出处理器明确报错，不会静默丢弃输出。AVFrame 的 time_base 描述处理节点输入的时间单位。扩展不得留下未排空线程或异步回调，也不得让异常穿过 C ABI。

扩展 DLL 与 extensions.json 随认证媒体包交付，只按清单加载；扩展使用同一组 FFmpeg DLL，不携带另一份运行库。入口、指纹或 ABI 错误使服务准备失败。模块保留到 worker 退出，实例随轨道释放；不支持运行中热替换，也不提供扩展间安全隔离。

示例 `examples/flip` 使用 libavfilter hflip。引擎 SDK 安装后可在仓库外独立构建它。SDK 的 ztmedia_write_extension_bundle 生成扩展打包清单；示例构建后在 Release/extension-bundle.json 输出该文件。将它传给 build-media-windows.ps1 的 -ExtensionBundle，或 CMake 的 MEDIA_EXTENSION_BUNDLE，即可替换 worker 的扩展集合，无需修改服务源码。清单为 source 与 path 对象数组，source 可相对清单目录，path 必须位于 extensions/；服务启动仍验证 DLL 内的真实 ABI 和构建指纹。

## 构建、预览和测试

从仓库根调用 `.agents/skills/zto-build/scripts/build-media-windows.ps1 -QtRoot <Qt SDK> -ApiSdk <宿主 SDK>`。首次调用通过 build-ffmpeg-windows.ps1 构建受控依赖；可用 `-FfmpegRoot` 指向已验证依赖 SDK，`-OpusSource` 指向离线 Opus 源码。工具使用固定源码 URL 和 SHA-256；不修改系统 PATH 或 Qt Kit。

交付目录为 `build/dist/Z2-media-api-v2/`、`build/dist/Z2-media-engine-sdk-v1/` 及各自 ZIP，媒体包为 `build/dist/plugins/z2_media_worker-v2.0.0.z2plugin`。媒体运行库、Qt 媒体后端和 CRT 只进入 worker 包，收集器显式使用同一组受控 FFmpeg 替换 Qt Kit 的同名库，并校验实际二进制指纹与许可证来源。

API SDK 的 `examples/preview` 是仅用于开发的预览插件，可显式选择来源，展示采集和 H.264 回送图像；不进入主程序发行包。它只使用公开宿主／媒体 SDK。

算法与通道测试位于仓库 `src/tests/media/`，媒体项目用 `MEDIA_BUILD_TESTS=ON` 配置；宿主集成测试用 `ZTO_BUILD_TESTS=ON`、`ZTO_MEDIA_PLUGIN_PATH=<媒体包>` 配置。分别设置 `ZTO_TEST_PHYSICAL_VIDEO=1`、`ZTO_TEST_PHYSICAL_AUDIO=1` 启用实际设备；`ZTO_MEDIA_SOAK_SECONDS=600` 启用十分钟测试。真实设备不可用会明确跳过，不等同于验收通过。

使用构建技能中的 run-media-tests.ps1 指定 QtRoot、TestExecutable、SupportBin 和 TestArguments；实际宿主插件加载测试还传 -HostRuntime build/dist/Z2-windows-x64 准备匹配宿主的应用目录基础 DLL。启动器检查 qwindows.dll 及本地 Qt Core 一致性，设置正确插件路径，并对无人值守测试设置退出时限。worker 自己的插件路径独立于测试进程，不继承父进程的 QPA 路径或 headless 平台选择。

本代仅验证 Windows x64。系统音频回环、GPU 共享、完整视频业务插件、任意处理图编辑器和其他平台交付不在范围内。

桌面共享交付中仍然开放的媒体侧限制见[当前遗留问题](limitations.md)。
