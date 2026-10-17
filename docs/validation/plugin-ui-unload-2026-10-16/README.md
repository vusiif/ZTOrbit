# 插件依赖提示、语音交互与媒体卸载验证

日期：2026-10-16。Windows x64，MSVC 2022，动态 Qt 6.8.3，应用 0.4.1／API 0.4；没有修改版本。测试使用独立临时设置与插件目录，不操作现有用户程序。

## 交互及音频

- 服务回归：24 项通过，覆盖共享提供者拒绝卸载、依赖者继续服务、禁用状态保持、消费者先卸载、停止失败与重试、取消、进程异常和资源回收。
- Windows 窗口回归：缺失依赖时提示并保持未加载；深浅主题中右键卸载媒体服务仅提示依赖者，两个插件继续加载。
- 语音源码回归：10 项通过，覆盖空昵称异步获取 ZeroTier ID、取消查询与获取失败、本地麦克风回放、恢复静音、关闭设置停止测试，以及左右键成员音量浮窗。QML 警告列表为空。
- 物理音频回归：实际输入／输出设备打开，采集 19 帧、接收回放 17 帧、向输出写入 145912 字节。输出总音量为 0，验证采集与播放链路，不以人工听感作为测试结论。

## 媒体插件残留调查

在同一个窗口测试进程中，从空插件目录开始，导入并手动加载媒体包，连续卸载／加载 6 轮；最后再等待 31 秒，让 Qt 默认线程池空闲线程有机会退出。内存单位为 MiB（1024² 字节）。

| 时点 | 私有内存 | 工作集 | 句柄 | 线程 |
| --- | ---: | ---: | ---: | ---: |
| 初始空插件 | 134.94 | 178.98 | 947 | 24 |
| 第 1 轮卸载后 | 137.04 | 180.78 | 951 | 25 |
| 第 6 轮卸载后 | 137.29 | 177.43 | 937 | 20 |
| 再空闲 31 秒 | 136.74 | 176.97 | 950 | 14 |

每轮都通过以下硬性检查：注册表无插件；媒体 PID 对应的进程句柄已报告退出；该进程实际运行的解包目录已不存在；宿主私有运行库目录与句柄计数均为 0；宿主模块中没有媒体 worker、Qt Multimedia、avcodec 或 avformat。

没有发现媒体进程、媒体模块或解包资源残留，也没有持续增长。最终私有内存较初始增加约 1.8 MiB，工作集减少约 2.0 MiB。用户报告的约 20 MB 差值在此受控环境中未复现，不能据此确定原运行环境的差值来源。QtTest、软件绘制、字体和模拟网络查询使本测试进程基线不同于正式主程序；不能用这里的绝对值替代用户的 80 MB 基线。线程与工作集会随空闲时间变化，单次任务管理器读数不足以判断泄漏。本次没有通过强制裁剪工作集掩盖占用，也没有修改媒体清理实现。

原始数据见 [media-unload-memory.csv](media-unload-memory.csv)。内存测量是显式启用的慢测试，避免把默认窗口回归推过原有超时：

```powershell
$env:QT_QUICK_BACKEND = 'software'
$env:ZTGUI_TEST_MEDIA_MEMORY = '1'
& ./cmake-build-qml/Release/zerotier_gui_tests.exe mediaOnlyUnloadMemory -platform windows
```

## 构建范围

主程序通过 `.agents/skills/ztgui-build/scripts/build-windows.ps1` 构建并生成 `dist/windows-x64.zip`，文件版本与产品版本均为 0.4.1；ZIP 的 191 个文件与部署目录逐项 SHA-256 一致。API 输入没有变化，复用已安装 SDK 和 ZIP。语音 UI 测试源码统一放在 `src/tests/voice-chat/`。

语音插件从 `src/plugins/voice-chat/client/` 独立构建，产物为 `dist/plugins/ztgui_voice_chat.ztplugin`（178443 字节，插件 0.2.0、API 0.4、Windows x64／MSVC／动态 Qt 6.8.3）。使用已安装的宿主 API 0.4 与媒体 API 1；运行需要匹配宿主和现有的 `media >=1.0.0 <2.0.0` 插件（`media.audio / 1`）。媒体包没有重编，主程序和 SDK 也没有因这次独立插件构建而重新打包。

```powershell
& ./.agents/skills/ztgui-build/scripts/build-plugin-windows.ps1 `
    -PluginDirectory src/plugins/voice-chat/client -Target ztgui_voice_chat `
    -QtRoot ./cmake-build-tools/Qt/6.8.3/msvc2022_64 `
    -ApiSdk ./dist/ZTGUI-api-v0.4 -AdditionalSdk ./dist/ZTGUI-media-api-v1
```

包认证检查通过，发布包与构建包 SHA-256 均为 `4E96EA62247DE2DFEA2BA543D5BA2ADC90CE60D83C6842D7DCF470E0409E3E67`。新包实际动态加载、页面创建、音频设置弹出、模拟服务器连接、卸载、重载与偏好恢复回归通过；使用新包的深浅主题依赖卸载拦截回归也通过。对应日志为 `voice-package-result.txt` 和 `voice-package-host-result.txt`。
