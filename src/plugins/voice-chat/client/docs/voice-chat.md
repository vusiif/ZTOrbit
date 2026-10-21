# 语音客户端插件

语音插件 `0.3.0` 是独立 Qt Quick/QML 项目，只链接安装后的宿主 API `0.5` 和媒体 API `2.x`。ZTPK3 清单声明前置 `media >=2.0.0 <3.0.0` 及 `media.session / 1`、`media.clip / 1`；DLL 不导入 Qt Multimedia、FFmpeg 或 Opus，也不链接媒体实现。媒体项目及打包见[媒体文档](../../../media/client/docs/media.md)。缺少 `PluginStorage` 事务扩展或媒体 clip 服务的宿主会得到明确的初始化错误，不会回退为写注册表或静默省略提示音。

VoicePage.qml 展示房间、成员和音频配置，页面持有 VoiceController；VoiceSession 保留现有 TCP/UDP 与房间业务。网络使用事件循环异步运行，音频实现全部在共享媒体工作进程中。VoiceAudio 仅在语音项目内部将业务成员映射为通用发送／接收音轨，不作为兼容 API 对外提供。切换菜单保留页面和会话。插件的独立异步停止接口先断开网络并关闭媒体租约，确认排空后才允许页面、工厂及 DLL 销毁。

首次服务器地址为空；填入地址和打开面板不会联网，点击连接才建立会话。配置保存在宿主的 `plugins-data/id-voice-chat/config.json`，语音自行维护格式版本和业务字段（`address`、`nickname`、`volume`、`inputVolume`、`muted`、`deafened`、`input`、`output`）；不再恢复已废弃的 `port`。音量限制在 0–200，默认输出 80、输入 100；设备 ID 使用带校验的十六进制字符串，空串表示系统默认。正常编辑只通过 `updateFile` 修改本次变化字段，不使用整份旧快照覆盖其他进程的新值；写失败保留最后成功的持久状态并提示。首次升级时通过同一事务从当前／历史 Native `voice-chat/*` 补缺项，目标已有值优先；目标提交并重新读取校验成功后，只删除实际补缺的当前／历史来源键，删除后再 `sync()` 并重新读取确认，冲突、未知和未迁移的键保留。清除失败会显示警告并保持导入资格开放，下次加载重试；全部成功后调用 `completeLegacyImport` 关闭导入资格。配置损坏时保留原文件并阻止自动覆盖。

成员备注保存在 `plugins-data/id-voice-chat/member-remarks.json`，按小写十六进制 ZeroTier ID 匹配，并用 `updateFile` 只更新本次成员；两个用户修改不同成员时互不覆盖。备注属于持久业务数据，永远不作为缓存淘汰。

异步设备枚举完成前保留已保存的设备 ID。设备 DTO 不暴露媒体对象，热插拔由 worker 通知；缺失的设备在界面回退到系统默认；列表刷新和热插拔不重启采集，需要用户重新选择设备或明确停止后启动。

## 房间提示音

房间事件在会话协议层产生，不根据 `roomChanged` 次数或人数差播放：自己成功 join/create 后播放一次上行双音；首份成员快照只建立基线，之后仅对同一连接世代、同一房间内非自己 session ID 的成员集合做差分，远端成员进入播放上行音、离开播放下行音。昵称、静音、排序和重复快照不触发；同人数一进一出分别识别。离房、断线、重连和停止清空基线与待播队列，旧回调失效；本轮不增加自己退房提示。仓库 `voice_ui_tests` 通过真实控制协议夹具覆盖 create/join 成功、失败响应、挂起时断开取消、同 session ID 重连后的基线重建，以及 `roomReset`／停止后旧 `finished` 不再启动播放。

两段约 160 ms 的提示音随 DLL 资源交付：`qrc:/ztgui/voice-chat/sounds/join.wav`（660→880 Hz）与 `leave.wav`（880→660 Hz），48 kHz/PCM16/单声道，带短淡入淡出。生成脚本和 CC0 授权说明见 [sounds](../sounds/README.md)；不读取源码树、当前目录、系统音效或网络。插件在内存中读取资源，经 `media.clip / 1` 交给媒体工作进程，语音 DLL 不链接 Qt Multimedia／FFmpeg／Opus。

播放队列最多 8 项（含正在播放的 1 项）：突发时只合并等待队列中相邻同类事件，不跨加入／离开合并；等待用单调时钟计龄，超过 2 秒丢弃；满队列先丢最旧待播项，保留当前与最新状态。正常逐次播放，`finished`／`failed` 事件都推进队列；失败事件即使携带空或未知 clip ID，也会结束当前待播请求并继续下一项，不会把状态卡在“starting”。每次播放前使用最新输出设备和主音量；麦克风静音不影响提示音，禁听会停止当前 clip 并清空队列；换设备时停止旧输出上的 clip，后续项使用新设备。插件停止时先停止提示音，再等待 session 与 clip 两个租约都排空后才报告完成。

昵称留空时通过宿主公开的 ZeroTier 查询异步取得本机 ID 作为昵称；查询失败提示手动填写，取消连接或卸载会取消待处理查询。音频设置的麦克风测试将采集的 Opus 帧送回本地媒体服务播放，测试期间不发送采集帧到房间；再次点击、关闭设置、离开页面或卸载会停止测试并清除回放缓冲。测试临时绕过麦克风／耳机静音，保留已保存的静音与音量偏好，结束后恢复原状态。成员音量只能通过成员行的左键／右键浮窗调整；成员退出时浮窗自动关闭。

## 协议与行为兼容

成员行浮窗支持保存或清空本地备注，显示顺序为本地备注、服务器昵称、ZeroTier ID。备注通过公开 `PluginStorage::updateFile` 原子保存到 `plugins-data/id-voice-chat/member-remarks.json`，按小写的十六进制 ZeroTier ID 匹配，不上传服务器；退出房间、重连或修改昵称不影响备注。损坏文件保留且禁止覆盖，写入失败保留原备注并显示错误。

连接时通过公开 ZeroTier 服务查询本机 ID，`hello.data.ztid` 和服务器成员列表的 `ztid` 是新增可选字段（十位十六进制），业务协议版本仍为 1。旧服务端忽略新字段；连接旧服务端时，只有昵称本身为 ZeroTier ID 的成员能够设置稳定备注，其余成员的备注入口禁用。自报 ID 沿用现有可信内网模型，不提供身份认证。

控制 TCP 默认 `18100`，支持域名／IPv4 的显式端口、`[IPv6]:端口` 及裸 IPv6；UDP 端口来自服务器握手。地址使用公开 SDK 的 serverEndpoint；旧独立端口偏好不覆盖默认值。TCP/UDP 直接连接，不使用 HTTP/SOCKS 代理。不会自动连接、自动重连或在媒体进程重启后恢复录音。

房间开放且临时，一个会话只能加入一个房间，最后成员离开后删除。语音协议与路由 ID `voice-chat` 保持，客户端使用同项目 `shared/Protocol.h`；Java 服务端在 `VoiceProtocol.java` 中按字节兼容实现并继续仅转发。Java 迁移后的部署方式见[服务端文档](../../server/docs/voice-chat.md)。

入房并完成 UDP 注册后，才通过媒体 API 启动音频。PCM 转换为单声道 48 kHz，Opus 为 20 ms、24 kbps；每个发送者独立解码、约 60 ms 缓冲和丢包补偿，混音后限幅输出。输入、总输出和成员本机音量仍为 0–200%；麦克风静音不发送，耳机静音仍维护接收流。退房／断线立即请求停止音频，卸载等待远端资源关闭。

面向 ZeroTier／可信内网，尚无账户、密码、房主控制、TLS、录音、回声消除或自动重连。ZeroTier 节点列表不等同于应用联系人或媒体服务发现。

## 独立构建

```powershell
cmake -S client -B build-client "-DCMAKE_PREFIX_PATH=$hostSdk;$mediaSdk;$qtSdk"
cmake --build build-client --config Release --target ztgui_voice_chat ztgui_voice_chat_package
```

在 `voice-chat/` 项目根或仓库外完整副本执行，只需 client/、shared/ 和安装后的 SDK。Qt 6.8+ 的 Quick/Qml/Network 与 MSVC／架构／构建模式须匹配宿主；语音项目不再下载或编译 Opus。

仓库测试 `src/tests/voice-chat/VoiceUiTests.cpp`，覆盖协议边界、配置和面板交互；配置时加 `-DVOICE_BUILD_TESTS=ON`，构建 `voice_ui_tests`，用 `voice_ui_tests.exe -platform windows` 验证原生界面。测试仅在完整仓库启用；正式插件可复制到仓库外后使用安装 SDK 独立构建。媒体算法测试由媒体项目自己的 tests/ 维护，宿主 src/tests/ 仅保留跨插件集成及通用框架测试。

仓库通用入口：

```powershell
& ./.agents/skills/ztgui-build/scripts/build-plugin-windows.ps1 -PluginDirectory src/plugins/voice-chat/client -Target ztgui_voice_chat -QtRoot $qtSdk -ApiSdk $hostSdk -AdditionalSdk $mediaSdk
```

正式产物为 `build/dist/plugins/ztgui_voice_chat-v0.3.0.ztplugin`（版本从包声明读取；中间构建文件保留固定名），同时安装独立媒体包；缺少或明确禁用媒体时不会执行语音入口。卸载语音保留包和用户数据，自动媒体无其他占用时退出；用户单独加载的媒体保持至用户卸载。旧 0.1.0 进程内媒体包必须迁移，不能用旧常驻路径回退。

Windows 原生 UI、真实设备的本机零增益回送及 2 轮暖机／20 轮模块检查见[本轮验证](../../../../../docs/validation/plugin-services-2026-10-15/README.md)。未在本轮重新连接生产服务器或进行多人听感验收；Linux 客户端和 macOS 的 worker 部署／权限未验证。
