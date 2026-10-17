# 通用媒体引擎验收（2026-10-17）

环境：Windows x64、MSVC 2022 19.44、动态 Qt 6.8.3。根 VERSION、Windows 文件版本与应用版本均保持 0.4.1，宿主 API 0.4；媒体提供者与 Media API 2.0.0，Engine SDK 1.0.0，语音客户端 0.3.0。

## 实现与接口

公开 Media API 仅传递 Core/Network 与服务契约；MediaTypes 可只用 Qt Core，MediaQuick 为可选呈现组件。引擎和 FFmpeg 留在 worker，扩展只在 worker 内使用 FFmpeg 对象。旧 AudioClient、media.audio 契约及旧语音引擎实现不作为兼容入口保留。

设备对象归专属事件线程，会话处理归独立执行线程。来源回调绑定来源自己的 QObject 上下文；关闭先解绑并排空设备回调，再销毁处理目标。音视频采集使用共同单调时钟，播放时间线按远端时钟域／syncGroup 映射，不以处理线程的排队时间充当采集时间。设备列表刷新不自动重新开启麦克风。

数据连接按租约认证，消息分块并限制重组与排队字节；会话执行队列同时限制请求数量和字节数。预览使用三槽共享内存，未读取的旧帧可以替换，读取中的槽位不会覆盖，通知与槽位序号必须匹配。SDK 的帧回调可关闭或销毁会话，直接回调持有宿主 DLL 生命周期守卫。

## 实测结果

| 检查 | 结果与证据 |
| --- | --- |
| 引擎／DTO／通道／共享槽位／Opus／H.264／时钟域映射／扩展 ABI | 12 passed，engine.txt |
| 真实 worker 与 ASan 消费者：隔离、关闭／销毁回调、崩溃失效、慢消费者 | 全部通过，media-integration.txt |
| 摄像头、显示器、窗口 H.264 回送与物理麦克风录放音 | 全部通过，无设备跳过，media-integration.txt |
| 语音设置、备注、麦克风测试、设备变更不重启采集、原生页面 | 12 passed，voice-ui.txt |
| 通用依赖／停止／崩溃／超时与取消 | 24 passed，plugin-services.txt |
| 包容器与运行库边界 | 10 passed，packages.txt |
| 实际语音插件加载／卸载、无媒体模块残留、点击才联网、真实服务器房间与采集回送 | 7 passed，无跳过，voice-integration.txt |
| 仓库外 SDK 预览消费者：采集预览与 H.264 回送均渲染红色测试区域 | 3 passed，preview-consumer.txt |
| 仓库外完整媒体项目及独立 FFmpeg 扩展构建、清单封装并装入 worker | 通过，external-*-build.log、external-extension.txt |
| 只依赖 Types 或 MediaApi 的仓库外消费者 | 构建且运行返回 0；配置断言未引入 Qt Quick／Multimedia |

语音联调使用本机 WSL 内的现有 Linux 服务端和独立测试数据目录、随机端口，未访问生产服务器、修改安装服务或更改服务器协议。物理音频回送输出音量设为零，验证计数，不保存录音。

视频稳定性目标为 1280×720、30 fps、2 Mbps、软件 H.264。连续 600 秒实际解码 17040 帧，平均 28.4 fps；worker 私有内存 59.63–68.24 MiB，结束约 59.67 MiB。最终队列深度 0，峰值 2。原始计数及每秒内存见 video-soak.txt、video-soak.csv、video-soak-summary.json。测试期间存在其他构建负载，此结果不作为任意机器固定性能保证。

## 交付校验与边界

主程序、宿主 SDK、Media API、Engine SDK 的 ZIP 与对应目录逐文件 SHA-256 一致，见 archives.json；媒体与语音包已完整认证检查，版本／服务及 SHA-256 见 media-package.json、voice-chat-package.json。主程序 ZIP 不含 Qt Multimedia、FFmpeg、媒体扩展或业务插件。

测试启动器检查 Qt Core 与平台插件，配置 QML 路径；宿主插件测试使用实际 HostRuntime 的应用目录基础 DLL，防止 DLL 搜索策略导致 Basic 样式无法加载。测试函数时限覆盖十分钟测试，外层进程仍有明确超时，避免启动错误对话框无限阻塞。

Engine ABI 1 为零或一帧输出的帧处理扩展，音频保持协商的 960 样本块；不实现任意拓扑、完整视频／屏幕业务插件、系统音频回环、GPU 跨进程共享、扩展热替换和其他平台验收。帧数据路径与工作进程隔离不构成安全沙箱。

## 清理限制

已确认的 dist/*.previous-* 构建备份和失败 SDK 暂存目录的递归删除被自动审批拒绝。进一步收窄到单个、完整字面路径的备份目录后仍返回 blocked by policy，工具没有提供更具体原因。未绕过策略删除，备份保留；正式交付包与校验记录不依赖这些目录。
