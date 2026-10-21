# 客户端存储、插件清理与语音提示验证（2026-10-18）

本次验证对应 GOAL.md 的 P1–P5 实现，应用版本保持客户端 `0.5.1-alpha`／API
`0.5-alpha`，未修改 `VERSION`／`SERVER_VERSION`，也未改变 API 代际。服务端业务和线协议
未修改。

## 范围

- 统一存储根、安装路由、只读回退、宿主 INI 设置与旧 Native 设置白名单迁移。
- 稳定插件 ID 数据目录、旧显示名目录歧义迁移、ID 租约、所有权／删除墓碑、插件数据
  事务与一次性旧配置导入资格。
- 解压缓存所有权、会话索引、崩溃恢复与隐藏正常退出清理入口。
- 日志轮转／保留、插件业务缓存保留、设置页“存储与清理”与经确认无主数据删除。
- ZeroTier 成员不可达 ICMP 结果显示 ASCII `-`。
- 媒体 `media.clip / 1`、语音内嵌提示音、成员集合差分与有界合并队列。

## 自动化结果

| 验证 | 命令／入口 | 结果 |
| --- | --- | --- |
| 存储专项 | `storage_tests.exe` | 13 通过，0 失败；覆盖选根／路由、旧设置迁移与损坏保留、个人 profile 复制、ID 目录迁移、租约、墓碑、无主扫描、日志／缓存保留 |
| 包与缓存会话 | `plugin_package_tests.exe` | 11 通过，0 失败；含缓存会话所有权、崩溃回收与隐藏清理入口 |
| 宿主核心 | `core_tests.exe -platform offscreen` | 全量通过；含稳定 ID 文件服务、`updateFile` 动作区分、异常保留、两进程并发计数 240 |
| 插件加载 | `plugin_loader_tests.exe -platform offscreen` | 18 通过，0 失败；含删除顺序、租约阻止与待删包 |
| 媒体引擎 | `media_engine_tests.exe -platform offscreen`（PATH 含受控 FFmpeg bin） | 13 通过，0 失败；含 WAV 边界／时长／对齐拒绝 |
| 媒体集成 | `media_integration_tests.exe clipServiceAcceptsAndFinishesShortAudio` | 通过；真实 worker 接受短音并发出 `finished` |
| 媒体 worker 全量 | `ctest -R media_worker_integration` | 通过 |
| 语音 UI | `voice_ui_tests.exe -platform offscreen` | 16 通过，0 失败；含配置迁移、备注字段合并、成员差分、队列合并／上限／过期、禁听清理 |
| 真实语音包 | `voice_plugin_tests.exe -platform offscreen` | 5 通过，3 条件跳过（真实服务器／听感） |
| GUI 回归 | `zerotier_gui_tests.exe -platform offscreen` | 31 通过，0 失败，3 条件跳过；含“存储与清理”专项 |
| 其它宿主 CTest | `ctest -C Release -E gui_regressions...` | 通过；`relay_end_to_end` 在重负载并发下曾一次超时，单独重跑通过，属于既有网络时序波动 |

## 构建产物

由构建技能 `build-windows.ps1 -RebuildApi -BuildMediaPlugin -BuildVoicePlugin` 重建：

- `build/dist/windows-x64/` 与 `windows-x64.zip`（宿主；不包含业务插件）。
- `build/dist/ZTGUI-api-v0.5-alpha/` 与 ZIP（新增 `ztgui/PluginStorage.h`；`AudioClipClient` 属于媒体 SDK）。
- `build/dist/ZTGUI-media-api-v2/`（新增 `ztmedia/AudioClipClient.h`）。
- `build/dist/plugins/ztgui_media_worker-v2.0.0.ztplugin`（提供 `media.session` 与 `media.clip`）。
- `build/dist/plugins/ztgui_voice_chat-v0.3.0.ztplugin`（内嵌两段提示音，依赖两个媒体服务）。

## 未验证边界

- macOS 客户端、真实多用户 ACL／只读安装下的程序目录探测、跨用户并发删除未执行。
- 真实崩溃后由隐藏清理进程回收缓存仅通过单元级会话测试验证，未在进程崩溃注入下执行。
- 原生 Windows 托盘、真实多人听感、长时间语音和真实音频设备热插拔未在本轮重新验证。
- Java 服务端及归档 C++ 服务端未重编；它们不属于本轮客户端存储目标。
