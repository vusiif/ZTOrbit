# 插件服务、依赖与媒体隔离

当前客户端 `0.5.1-alpha`、客户端 API `0.5-alpha`，服务端 API `0.2-beta`。进程内入口和生命周期继续使用 `/3.0`；服务访问与停止采用独立扩展接口，因此不修改原 HostServices 虚表或 `PluginDefinition` 布局；服务租约另提供直接数据回调的寿命守卫。语音插件版本 `0.3.0`，媒体提供者与 Media API 为 `2.0.0`，Engine SDK 为 `1.0.0`。音视频能力、扩展 ABI 与构建交付统一见[媒体项目文档](../src/plugins/media/client/docs/media.md)。

## 包与兼容

`ZTPK3\n` 沿用 v2 分块 XChaCha20-Poly1305 和逐文件 SHA-256。认证头增加 `execution`、`dependencies`、`provides`，不能给旧格式添加这些字段后执行。路径、重名、junction、文件数、总大小、截断和尾随数据检查保持。共享格式密钥不是发行者签名。

- `execution.kind` 为 `in-process` 或 `worker-process`，认证的 entry 必须与文件清单唯一入口相同；worker 协议为 `ztgui-service/1`。
- ID 使用既有 ASCII 规则，比较不区分大小写；已知安装目录中同 ID 多包被拒绝。首版仅支持必需依赖、单版本和无环图。
- 版本为不含预发布后缀的三段语义版本；范围为以空格连接的 `= / > / >= / < / <=` 比较式，如 `>=1.0.0 <2.0.0`。不支持的表达式直接报错。
- 服务声明为 `{service, api}`，接口版本为正整数字符串；必须由明确的前置插件提供。当前提供者运行在工作进程；不支持多提供者选举、可选依赖或联网下载。
- 进程内 Qt 插件元数据的 dependencies/provides 必须与包完全一致；ID、显示名、插件版本、API 和 IID 再与实际入口核对。无菜单插件可以注册并显示在目录中。
- worker 不使用 Qt 插件 IID／宿主 Qt ABI 校验，只验证平台、架构、宿主 API 能力和 IPC；运行库不进入宿主。归档的 C++ 服务端保留 v1/v2 并拒绝 v3；Java 服务端只加载 `plugins/*.jar`，不使用 `.ztplugin`。

新宿主继续加载普通 v1/v2 插件；旧格式不能声明服务依赖。直接导入额外 Qt 模块／后端的旧媒体包被拒绝，要求迁移重编。普通旧插件的非 Qt 私有库仍可能使用进程级固定寿命，这不是媒体可卸载保证。旧宿主通过新 magic 拒绝 v3。若先前进程曾加载常驻媒体库，必须退出并启动干净进程。

## 加载、占用与停止

后台认证完整目录，包括 v1 的实际密文；认证后的清单才能进入三色 DFS。先验证整个闭包的缺包、版本、服务、禁用状态和循环，再执行任何业务入口。计划保存认证包快照，提取时再核对内容，防止并发扫描或替换改变已经解析的提供者。DLL 加载仍在后台，QObject、注册表和生命周期留在 GUI 线程。

共享节点合并为一次操作；依赖者分别通过事务和已加载依赖关系占用提供者。重复的同目标加载合并。用户直接加载提供者保存显式保留意图；自动启动的提供者在没有事务、消费者和远端资源时自动停止。用户明确禁用的前置插件不会被消费者加载悄悄启用。只有直接手动加载该提供者成功后才清除禁用。

`util::ServiceAccess` 是 HostServices 的独立 RTTI 扩展，`pluginServices().acquire(provider, service, api)` 仅暴露清单已经解析且就绪的服务。租约绑定消费者实例和提供者代次；服务调用、事件和 close 都绑定 GUI 线程的 QObject 上下文。上下文销毁会清除函数对象；close 幂等，完成前等待在途调用和远端资源释放，不把“发送了关闭命令”视为完成。

长期业务实现 `<ztgui/PluginStop.h>` 的 `PluginStopInterface/1.0`：`requestStop(context, completed)` 在 GUI 线程发起，异步完成后才销毁页面／工厂、清理 QML 缓存、执行最终 `onDestroy()` 并卸载 DLL。停止失败保留对象、DLL 和提供者，显示错误；可对失败的停止显式重试，旧停止回调由实例及尝试代次拒绝。最终清理／模块卸载失败不会伪报成功。退出等待同一停止流程；正常流程没有 GUI 同步 wait。

显式卸载仍被依赖的提供者时，加载器直接拒绝并返回依赖者名单，界面仅显示提示框；提供者及其消费者继续保持原状态，禁用／保留设置也不变。正在加载的消费者事务同样阻止提供者被手动卸载。用户先卸载消费者后可再卸载提供者；正常退出与提供者异常仍按反向依赖顺序停止。缺失前置依赖时弹窗提示，目标保持未加载，不执行入口。加载取消及初始化失败只清理无其他事务／消费者或用户意图占用的资源。直接析构加载器仅是紧急兜底：未完成异步停止的消费者 DLL 保留，宿主拥有的工作进程仍被终止；正常调用方必须等待 shutdownFinished。

## 工作进程与传输

每个提供者实例只启动一个进程。Windows 以挂起方式创建进程，加入带 KILL_ON_JOB_CLOSE 的专属 Job 后才恢复主线程；握手同时核对随机连接凭据、PID、代次、协议和提供的服务。服务准备和两个连接握手都完成后才发布 Ready。子进程监视宿主句柄，宿主退出／崩溃不会留下孤儿树。停止先优雅退出，超时只终止宿主拥有的 Job；确认整个树退出后才释放提取锁和目录。

控制与二进制数据使用两个独立的本机 socket。消息包含有界 JSON 信封和独立二进制负载，不使用 Base64 音频；每个租约使用连续序号，在 worker 中有界排序，保证跨通道的 start/receive/stop 顺序。每帧 JSON 上限 64 KiB，二进制上限 256 KiB，每 socket 待写上限 1 MiB，最多 128 个在途请求，调用超时 10 秒，启动握手超时 15 秒。每个消费者最多 32 个租约，worker 最多 32 个服务会话。

慢设备初始化使用可选的延后就绪协商：worker 在 start 前调用 deferReady，两条 hello 都声明 `preparing:true`；宿主完成认证后回复 `deferredReady:true`，进入独立的60秒准备阶段。worker 的 onConnected 在两条 accepted 收到后运行，可启动后台准备，完成后在事件线程调用 markReady，发送带代次的 `ready`。宿主此时才发布服务并继续消费者事务。准备期间 IPC 继续处理关闭请求；未声明 preparing 的旧 worker 保持原握手即就绪语义。使用该扩展的 worker 不接受缺少协商确认的旧宿主，两端须同时更新。

关闭先禁止新调用、清除事件和消费者回调，待两个通道的在途回复排空后再释放远端会话；worker 的 onRelease 返回前须结束该会话拥有的任务。进程异常会使服务和代次失效，终结所有旧请求，并异步停止消费者；不会自动恢复录音。新加载使用新代次和新租约。通用分块数据通道和 Windows 共享帧缓冲由公开工具与媒体 SDK 实现，GPU 共享和完整屏幕业务插件仍未提供；fixture.video 仅验证调度。

worker 自带 Qt 基础库、媒体后端、第三方库及 MSVC 运行库。PATH、Qt 插件路径、工作目录和凭据只设置在子进程环境；宿主不调用 activateRuntime，不把 worker 目录加入自己的 DLL／Qt 搜索路径。隔离提供生命周期和故障边界，不构成安全沙箱。

Windows worker 的认证入口放在 `runtime/bin/<worker>.exe`，与 Qt／MSVC DLL 同目录；仅设置 PATH 无法保证启动阶段优先使用包内运行库。`runtime/bin/qt.conf` 是允许的 worker 配置资源，固定 Qt 插件路径为包内 `runtime/qt-plugins`。旧 `service/<worker>.exe` 布局需用更新后的 SDK 重新封装；普通进程内插件仍不得将业务入口或资源放入保留运行库目录。启动失败记录最初错误与握手通道状态，强制清理不会覆盖原始原因。

## 项目与构建

- 宿主 SDK：`src/client/api/`；通用 wire 与 worker 工具：`src/shared/include/ztprotocol/ServiceWire.h`、`ServiceWorker.h`。
- 媒体独立项目：`src/plugins/media/client/`；可安装 API：其 `api/`；引擎：其 `service/`；[媒体文档](../src/plugins/media/client/docs/media.md)。
- 语音独立项目：`src/plugins/voice-chat/client/` 及同项目 `shared/`；[语音文档](../src/plugins/voice-chat/client/docs/voice-chat.md)。
- 自动化与夹具：`src/tests/PluginServicesTests.cpp`、`src/tests/plugins/`；宿主不包含媒体、语音或屏幕共享 ID 分支。

主程序与宿主 SDK 用项目构建技能；媒体 API／worker 用 `build-media-windows.ps1`；语音用通用独立插件入口并传 `-AdditionalSdk build/dist/ZTGUI-media-api-v2`。普通构建不自动编译业务插件。详细交付、安装 SDK 的仓库外构建及验收数据由[本轮验证记录](plugin-services-validation.md)维护。


## 租约直接数据通道

通用 Worker 接受 `$channel`，返回绑定租约、代次和随机凭据的本机端点。客户端使用公开 `ztprotocol/BulkChannel.h`；worker 通过 onData/sendData 接收／发送不透明元数据和二进制。64 KiB 分块重组为最多 8 MiB 消息，待写字节有界；不承担媒体业务。直接回调持有 ServiceLease::callbackGuard。需要异步排空的提供者实现 onReleaseAsync，并在任务与来源回调结束后完成 Reply；不能把发起销毁视为资源已释放。媒体共享帧契约由其独立 SDK 管理。
