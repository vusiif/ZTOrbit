# 验证记录索引

本目录按日期归档专项验证、原始输出、截图和发行产物哈希。每条记录只代表当时的环境与版本；目录内的原始证据不删除、不改写，结论以对应记录和原始文件为准。

## 专项验证

| 日期 | 主题 | 记录 | 范围 |
| --- | --- | --- | --- |
| 2026-10-13 | 0.3.2 资源占用 | [resources-2026-10-13](resources-2026-10-13/README.md) | 历史测量，不代表当前媒体 worker 行为；[原始数据](resources-2026-10-13/resource-measurements.json) |
| 2026-10-14 | 0.3.3／API 0.3 运行依赖分离 | [plugin-runtime-2026-10-14](plugin-runtime-2026-10-14/README.md) | 旧媒体运行库常驻主进程的证据 |
| 2026-10-15 | 插件服务迁移 | [plugin-services-2026-10-15](plugin-services-2026-10-15/README.md) | 迁移总结；[实施交接](plugin-services-2026-10-15/implementation-plan.md)；[产物审计](plugin-services-2026-10-15/artifacts.json) |
| 2026-10-15 | worker 启动布局修正 | [plugin-worker-startup-2026-10-15](plugin-worker-startup-2026-10-15/evidence.json) | `runtime/bin` 布局与主线程恢复 |
| 2026-10-15 | 慢设备准备阶段 | [plugin-worker-preparation-2026-10-15](plugin-worker-preparation-2026-10-15/evidence.json) | 延后就绪与 60 秒准备期限 |
| 2026-10-16 | 依赖提示、语音交互与媒体卸载 | [plugin-ui-unload-2026-10-16](plugin-ui-unload-2026-10-16/README.md) | 含 6 轮媒体卸载内存调查 |
| 2026-10-17 | 插件管理、语音备注与渲染设置 | [plugin-management-2026-10-17](plugin-management-2026-10-17/README.md) | 插件列表、GPU 设置与语音备注 |
| 2026-10-17 | 通用媒体引擎 | [media-engine-2026-10-17](media-engine-2026-10-17/README.md) | 媒体 2.0、独立 worker 与 600 秒视频稳定性 |
| 2026-10-17 | 托盘待机轮询 | [tray-idle-2026-10-17](tray-idle-2026-10-17/README.md) | 隐藏窗口停止轮询与服务使用期 |
| 2026-10-17 | ZTServer Java 迁移 | [ztserver-java-2026-10-17](ztserver-java-2026-10-17/README.md) | 迁移矩阵与远端部署；[第二台部署](ztserver-java-2026-10-17/second-host.md) |
| 2026-10-18–21 | 客户端存储、插件清理与语音提示（已关闭） | [阶段归档](storage-voice-2026-10-21/README.md) | R01–R10、T01–T18、A01–A07 及最后三项缺口已关闭；首轮、跟进、审计、修复与原始证据统一归档，保留未验证边界 |
| 2026-10-21 | ZeroTier 网段 ping 与 TCP 中继观测 | [zerotier-network-2026-10-21](zerotier-network-2026-10-21/README.md) | 两端绑定虚拟源 IP，对 14 个已授权成员进行三轮探测；记录 TCP 回退状态、服务 TCP 连接、peer 底层路径及全部成功／失败回显 |

## 版本发行记录

- [0.5.1 / 0.2.1 构建与验证](release-0.5.1-0.2.1.md)（2026-10-21）与[产物记录](release-0.5.1-0.2.1-artifacts.json)：去除预发布后缀、SDK／配套插件重建、Windows 针对性加载与 Linux 105 项验证，以及未覆盖项和备份清理限制。
- [0.5.1-alpha / 0.2.1-beta 构建与验证](release-0.5.1-alpha-0.2.1-beta.md)（2026-10-18）与[产物记录](release-0.5.1-alpha-0.2.1-beta-artifacts.json)：当时的发行验证、产物哈希与未验证项；后续存储／语音交付见[阶段归档](storage-voice-2026-10-21/README.md)。
- [0.4.1 产物记录](release-0.4.1-artifacts.json)：上一版发行哈希；其中 Linux 服务端条目属于 Java 迁移前的旧 C++ 实现。

目录内的其余文件（`*.txt`、`*.csv`、`*.jsonl`、截图等）是对应记录的原始证据，不单独列出。当前契约见[插件服务](../plugin-services.md)、[插件运行依赖交付](../plugin-runtime-separation.md)、[QML 维护约定](../qml.md)与[维护约定](../maintenance.md)。
