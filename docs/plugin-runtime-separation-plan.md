# 媒体运行依赖随插件交付：实施交接方案

后续版本指令：开发者已明确升级到0.4.1/API0.4；下文0.3.3/API0.3是原实施基线，历史约束不再覆盖该明确升级指令。

状态：代码与发行产物已完成；已验证结果及未验证条件见 [运行依赖分离](plugin-runtime-separation.md)。本文保留原范围和验收条件，实机未验证项目不视为通过。

工作目录：`D:\User\Code\Github\Utils\zerotier-gui`。实现前保留全部现有未提交改动，不重置工作树。应用 `VERSION` 保持 `0.3.3`，API 保持 `0.3`，客户端入口和生命周期 IID 保持 `/3.0`，服务端入口 IID 保持 `/2.0`。

## 1. 最终目标及范围

主程序只编译和部署自己的基础依赖。语音插件独立编译、收集媒体运行依赖，将它们封装进一个 `.ztplugin`；用户导入这个文件即可加载语音插件，不必向主程序目录手工复制媒体 DLL。

采用动态 Qt，共用宿主的 Core、Gui、Qml、Quick、Network 等基础库；Qt Multimedia、媒体后端及它们新增的运行依赖随插件包交付。Opus 继续静态链接进语音插件。主程序保留自己的 libsodium 依赖。

实施通用的多文件插件包和依赖管理器，宿主不得出现 `voice-chat` 特判，也不链接 Qt Multimedia。首次完成本方案需要更新主程序和 SDK；此后在 API、工具链及宿主运行库契约兼容时，更新语音插件只需重新编译和替换插件包。

本次实际交付覆盖 Windows x64 客户端，以及 Linux 服务端的封装工具和包兼容性。Linux 语音服务端没有录放音需求，不增加 Multimedia/FFmpeg。不扩展到 Linux 客户端或 macOS 媒体依赖部署；这些平台必须明确标注未验证。

不改变语音协议、设备枚举初始化时机、插件生命周期、音量计算、配置目录及数据迁移。插件 ID 仍为 `voice-chat`，显示名仍为“语音聊天”。不增加独立音频进程，也不保证插件卸载时释放全部 Qt 媒体后端内存。

## 2. 已核对的起点

| 位置 | 当前行为 | 实施方向 |
| --- | --- | --- |
| `src/plugins/voice-chat/client/CMakeLists.txt` | 插件自己链接 Qt Multimedia 和静态 Opus | 保留链接关系，增加依赖收集与封装声明 |
| `.agent/skills/ztgui-build/scripts/build-windows.ps1` | 将现有语音 DLL 追加到主程序的 windeployqt 输入 | 移除追加逻辑，主程序部署完全不读取业务插件产物 |
| `src/shared/package/PluginPackage.{h,cpp}` | v1 加密一个动态库 | 增加 v2 多文件封装、校验及提取，保留 v1 读取 |
| `src/shared/package/PackageTool.cpp` | 命令工具只接收一个动态库 | 增加 bundle 输入、文件列表检查和运行契约 |
| `src/shared/package/PackagePlugin.cmake` | POST_BUILD 封装动态库 | 增加独立依赖收集及显式封装目标 |
| `src/client/main/plugins/PluginLoader.{h,cpp}` | 每次提取到独立目录，再 QPluginLoader 加载 | 分离业务插件目录与进程级运行依赖目录 |
| `src/server/main/ServerHost.{h,cpp}` | 服务端启动读取同一 v1 格式 | 接入通用 v2 读取，保持只有启动加载 |

目前主程序没有直接链接 Qt Multimedia。问题在部署链，不需要把现有 AudioEngine 从主程序搬走。

Qt 6.8.3 的媒体集成对象具有应用生命周期，后台工厂也会缓存插件信息。因此业务插件成功卸载后，媒体依赖仍可能被 Qt 使用。实施时保留运行依赖缓存和路径，不能沿用业务插件的立即删除逻辑。依据：[Qt 媒体集成源码](https://github.com/qt/qtmultimedia/blob/v6.8.3/src/multimedia/platform/qplatformmediaintegration.cpp)。

## 3. `.ztplugin` v2 格式

### 3.1 固定规则

- 新封装工具默认生成 v2；新版宿主同时读取 v1、v2。旧宿主拒绝 v2 是预期行为。
- v1 不补装、不猜测媒体依赖；新版精简宿主遇到旧语音包缺依赖时，显示缺少运行依赖及“使用新 SDK 重编插件”的提示。仍能满足依赖的旧插件继续加载。
- 明文 JSON 保留既有基础字段，新增 `entry`、`files`、`runtime`、`requiresHost`。封装版本独立于 API/IID，不修改公开 C++ 接口布局。
- 保留现有 XChaCha20-Poly1305 算法及共享格式密钥；它仍只是封装与完整性校验，不建立发行者授权体系。
- 本版不引入 ZIP/TAR 解包依赖、不压缩内部文件；采用下面的分块文件容器，避免整包解密的内存峰值。主程序/SDK 的发行压缩规则继续沿用项目规则。

### 3.2 字节布局与上限

1. 六字节 magic `ZTPK2\n`。
2. 四字节大端 JSON 长度；随后原始 UTF-8 JSON。JSON 上限 1 MiB。
3. 按 `files` 数组顺序写入各文件的加密块。明文块固定上限 1 MiB；最后一块为余数；空文件也有一个空明文块。
4. 每块是随机 24 字节 nonce 加密文及 16 字节认证标签。块长度由文件大小推导，不接受额外自报长度。
5. 每块 AAD 为“原始 JSON 字节 + 大端 uint32 文件索引 + 大端 uint32 块索引”；索引从 0 开始。每块使用新随机 nonce。

文件数量最多 512，单文件最多 256 MiB，总明文最多 512 MiB；总包大小按 JSON、文件大小和块开销计算并严格核对。拒绝截断、尾随数据、溢出和超限。`inspect` 不再把整个密文读进 QByteArray；读写和提取按块进行。

每个文件条目包含 `path`、`role`、`size`、`sha256`；`role` 取 `entry`、`runtime-library`、`qt-plugin`、`license`、`resource`。业务入口必须唯一，且与 `entry` 对应。完整认证、逐文件哈希及全部路径验证成功后，才允许执行任何提取文件；失败删除本次未激活的临时目录。

路径统一使用 `/`，只能为包内相对路径。拒绝绝对路径、盘符、UNC、空段、`.`、`..`、反斜线、NUL、Windows ADS/保留设备名/尾部点空格，以及平台规范化后重复的路径。只支持普通文件；输入端拒绝链接和 junction，输出端拒绝穿越链接。所有权限由宿主设置，不继承包内权限。

### 3.3 包内布局

```text
plugin/plugin.dll                     # Linux 为 plugin.so
runtime/bin/<私有运行库>.dll
runtime/qt-plugins/multimedia/<后端>.dll
licenses/<组件许可证>
```

语音 QML 继续内嵌在业务 DLL，不拆成外部 QML 模块。许可证随包封装，同时保留构建产物目录中的可读许可证文件；工具增加 `--list` 和 `--extract-licenses`，后者只导出许可证，不加载代码。

## 4. 宿主基础运行库契约与构建收集

### 4.1 共用契约

新增 `src/shared/package/HostRuntimeContract.cmake`，由宿主和对应 SDK 共用。生成、安装 `host-runtime-contract.json`，并把宿主端契约编译为资源；不能要求独立插件项目包含宿主实现或依赖已有 dist 主程序目录。

Windows 客户端基础 Qt 模块起点为宿主实际使用的 Core、Gui、Network、Qml、Quick、QuickControls2、Widgets、Concurrent，再递归计算这些 Qt DLL 的导入依赖。服务端起点为 Core、Network。模块起点与实际宿主链接配置保持一致，测试检查二者没有漂移。

契约包含平台、架构、编译器 ABI、Debug/Release、Qt 完整版本、基础 Qt DLL 文件名集合及 `qtBuildId`。`qtBuildId` 用同一算法由这些原始 Qt Kit DLL 的名称及 SHA-256 排序后计算；必须在 windeployqt 修改部署副本之前计算。两端 SDK 分别安装自己的契约。

插件包的 `requiresHost` 列出实际依赖的宿主基础库。带额外 Qt 模块或 Qt 后端的包必须匹配宿主 `qtBuildId`，避免私有 Qt ABI 和不同补丁构建混用。没有私有 Qt 运行依赖的包保留原来的 Qt 主次版本兼容规则。平台/API/IID 等既有检查继续执行。

所有宿主基础库、系统 DLL 和 CRT 由宿主/系统提供，禁止在插件私有目录再带一份。包声明提供基础 Qt 库时直接拒绝。不得把封装工具本身的 Qt Core、libsodium 或整个 Qt SDK 塞进业务插件。

### 4.2 SDK 工具与 CMake helper

增加 SDK 安装的 `CollectPluginRuntime.cmake` 和调用所需的平台辅助脚本；宿主/SDK 构建无需 Qt Multimedia，只有声明媒体依赖的插件构建才查找该模块及部署工具。

扩充 helper，保留旧调用可用：

```cmake
ztgui_package_plugin(ztgui_voice_chat
    HOST client ID voice-chat NAME "语音聊天" VERSION "0.1.0"
    API "${ZTGUI_API_VERSION}" IID "vusiif.ztgui.api.PluginInterface/3.0"
    RUNTIME_QT_MODULES Multimedia
    LICENSE_FILES "licenses/Opus-COPYING.txt=${opus_SOURCE_DIR}/COPYING")
```

新增 `RUNTIME_QT_MODULES`、`RUNTIME_FILES`、`LICENSE_FILES` 参数。其中显式文件使用 `包内相对路径=源文件路径` 映射；第三方库由插件声明，不假设 windeployqt 会发现所有第三方依赖。

Windows 收集流程：

1. 只对本次业务插件 DLL，在独立干净暂存目录运行匹配 Kit 的 windeployqt；禁用翻译与 QML 调试工具，不扫描宿主 QML。
2. 从结果剔除契约中的基础库、基础 QPA/控件插件、系统/CRT 文件，只留下新增模块、对应 Qt 插件及必要传递依赖；媒体后端保持当前 Kit 的正常部署结果，不硬编码 FFmpeg DLL 的版本文件名。
3. 用 PE 导入及 delay-import 检查依赖闭包。每个非系统导入必须由宿主契约或包内文件解析；Qt 工厂动态加载的后端按模块声明补充。缺文件则封装失败，不使用旧 bundle。
4. 收集 Qt、媒体后端、实际 FFmpeg 构建和 Opus 的许可证；找不到实际分发组件的许可证则停止并明确报错。
5. 生成 bundle 清单，调用 `ztplugin_pack --bundle-manifest ...` 输出一个 v2 文件。

helper 增加 `${target}_package` 显式目标：依赖业务库，每次显式调用都重新核对依赖清单并原子输出包。保留 POST_BUILD 生成行为；在未重新链接 DLL、但外部依赖或打包脚本改变时，显式 package 目标仍能正确更新包。避免两个规则并发写同一个包。

服务端语音使用同一 v2 容器，通常只有业务入口和许可证，不捆绑系统 Qt。暂不实现 Linux 私有媒体库部署；helper 在未支持的平台接到相应依赖声明时明确报错。

## 5. 通用运行依赖管理与热卸载

新增 `src/shared/package/PluginRuntime.{h,cpp}`，宿主私有使用，不导出进插件公共头文件；两端宿主复用包校验和运行契约逻辑。

### 5.1 两种目录、两种寿命

- **业务目录**：保持每次加载独立的 `load-*` 私有目录，放入口和业务资源。成功卸载并释放 QPluginLoader 后清理；卸载失败保留目录/句柄。
- **运行依赖目录**：新增 `runtime-*` 私有目录，按本进程内的运行文件清单指纹复用。指纹仅覆盖运行依赖及其契约，插件业务 DLL 更新不生成另一份相同媒体缓存。文件、目录及锁保持到进程退出，下一次启动清理遗留目录。

复用只针对完整性校验通过且同进程已登记的目录；不同运行进程使用不同私有目录，保留现有活动锁检查，不能清理其他进程仍在使用的文件。

### 5.2 Windows 加载顺序

1. 校验基本信息、完整容器、宿主契约、ID 冲突和全部依赖冲突。
2. 提取业务文件；创建或取得运行依赖目录；预先验证所需基础库确实可由宿主部署提供。
3. 宿主统一建立 DLL 搜索策略：`SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`；运行库 bin 使用 `AddDllDirectory` 登记。不得改工作目录、全局 PATH 或用户的 QT_PLUGIN_PATH。
4. 根据依赖图，用绝对路径和 `LoadLibraryExW` 的 `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` 预加载私有运行库，依赖先于使用者。Qt 后端留给 Qt 工厂加载，不提前实例化。
5. 使用 `QCoreApplication::addLibraryPath(runtime/qt-plugins)` 登记 Qt 后端根目录。保留这个路径至进程结束，不修改借用的 QQmlEngine 的 import path。
6. 通过现有 QPluginLoader 加载业务 DLL，维持清空 PreventUnloadHint 的行为，核对定义后进入现有 onCreate 流程。

依赖管理器按 Windows 不区分大小写的 DLL 基名登记 SHA-256、来源、句柄、目录及激活状态。相同内容复用；同名不同内容、基础 Qt 库覆盖、已有外部同名模块无法证明兼容时，在执行业务入口前拒绝，并提示重启或使用匹配构建。多个 AddDllDirectory 的搜索顺序没有保证，不能靠目录先后解决冲突。依据：[Microsoft AddDllDirectory](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-adddlldirectory)。

Qt 6.8.3 的 QLibrary 实际使用 LoadLibrary，因此必须验证上面的进程级搜索策略确实覆盖 QPluginLoader 及媒体后端的间接依赖，不能只测试直接 LoadLibraryEx 成功。依据：[Qt Windows 加载实现](https://github.com/qt/qtbase/blob/v6.8.3/src/corelib/plugin/qlibrary_win.cpp)。

### 5.3 卸载及失败行为

保留现有顺序：禁止新操作 → 离开活动页面 → 取消任务/订阅 → 异步等待初始化及回调完成 → 释放页面/工厂 → onDestroy → 核对真正卸载结果。

卸载后释放业务资源，运行依赖句柄及 Qt 路径继续登记。默认交给进程退出释放系统句柄，不能在 QApplication 析构之前 FreeLibrary 媒体模块；运行目录锁也不能因插件卸载而提前释放。

失败若发生在预加载前，清理未激活目录；已有任何依赖执行/加载后，保留进程登记和目录，不冒险删除映射文件。业务插件加载失败仍按现有生命周期配对清理，准确显示错误，不标记为已加载。

本进程首次激活一套媒体依赖会增加内存基线，之后加载同一套不应重复映射。录音、播放、Opus、线程、页面、回调与缓冲区仍应在插件卸载时释放。重启后才切换冲突版本的运行库；插件之间不得借用对方服务或依赖对方包。

## 6. 构建入口与产物

### 6.1 主程序

修改 `build-windows.ps1`，不再追加或读取任何业务插件 DLL。已有语音构建缓存是否存在，对主程序 ZIP 的内容都不得产生影响；新增回归检查覆盖这点。

继续使用干净暂存目录部署主程序，保留当前发行目录备份及用户数据。禁止在用户已有运行目录中直接批量删媒体文件。主程序目录/ZIP 不包含 Qt6Multimedia、FFmpeg、multimedia 后端或业务 `.ztplugin`。基础 Qt 和 libsodium 照常部署。

保留 `-BuildVoicePlugin` 作为显式组合构建兼容入口，但改为调用下述独立插件入口，主程序 windeployqt 不受其影响。

### 6.2 新增插件独立构建入口

新增 `.agent/skills/ztgui-build/scripts/build-plugin-windows.ps1`：

```powershell
& ./.agent/skills/ztgui-build/scripts/build-plugin-windows.ps1 `
    -PluginDirectory src/plugins/voice-chat/client `
    -Target ztgui_voice_chat `
    -QtRoot $qtSdk -ApiSdk dist/ZTGUI-api-v0.3
```

默认构建目录按插件及平台独立命名，可通过 `-BuildDirectory` 覆盖；输出目录默认 `dist/plugins/`，可覆盖。仅配置独立插件、构建指定业务目标及 package 目标、原子发布包和许可证。明确设置安装 SDK 的 `ZTGUIPluginApi_DIR`，避免旧缓存指向仓库内 API。

SDK 缺失/过期时停止并提示先更新 SDK；插件独立入口不得自动构建主程序。每步检查退出码，失败不发布旧产物、不写成功记录。

Linux 服务端仍可使用已安装 SDK 独立配置 `src/plugins/voice-chat/server`，构建 `ztserver_voice_chat_package` 并发布到 `dist/server-plugins/`；已有组合脚本同步支持 v2。

### 6.3 SDK 更新

两端 SDK 都安装新 helper、收集脚本、契约和封装工具；不把宿主加载实现导出为插件 ABI。SDK 内容指纹覆盖全部新增工具/契约输入、共享包实现及相关 CMake 文件。更新两端 SDK ZIP，即使 API 数字没变；在 SDK 元数据中记录支持的 package format 与工具内容指纹。

正式插件 install 仅安装 `.ztplugin` 和许可证；裸 DLL/SO 只留在构建目录用于诊断和收集依赖。

最终 Windows 主程序产物仍为 `dist/windows-x64/`、`dist/windows-x64.zip`；插件为 `dist/plugins/ztgui_voice_chat.ztplugin`；SDK 为现有 API 0.3 目录及 ZIP。服务端产物路径沿用当前约定。

## 7. 实施次序及代码落点

1. 阅读根 AGENTS.md、项目插件开发与构建 SKILL.md，记录 `git status --short` 和版本；不得覆盖已完成的多中转、热加载、设置页和语音音量修改。
2. 实现共享 v2 容器、inspect/list/许可证导出及块式校验；先以无依赖测试插件验证 v1/v2 共存。
3. 实现基础运行契约、SDK 安装、依赖收集 helper 和显式 package 目标；用安装后的 SDK 验证独立消费。
4. 实现共享依赖管理器，接入客户端 PluginLoader 和服务端 ServerHost，补齐运行目录锁与陈旧缓存清理。
5. 修改语音 CMake 声明、许可证收集和正式 install 规则；移除主程序打包对语音 DLL 的扫描；增加独立插件脚本。
6. 完成下面的验证，再生成主程序、两端 SDK、语音两端插件及相应发行压缩包。
7. 更新 `CHANGELOG.md` 未发布部分、`docs/plugin-api.md`、`docs/server-api.md`、`docs/maintenance.md`、语音两端项目文档及两项项目技能。尤其删除构建技能中“主程序保留媒体依赖”“读取现有业务 DLL 依赖”的旧约定。

测试新增到 `src/tests/`，优先扩充现有 `plugins/LoaderTests.cpp`、`plugins/VoicePluginTests.cpp` 和 `sdk-consumer/`、`server/sdk-consumer/`；需要隔离容器测试时新增 `PluginPackageTests.cpp`。协议与音频既有实现无改动时，不扩展其功能范围。

本机已验证 Windows Qt Kit 为 `cmake-build-tools/Qt/6.8.3/msvc2022_64`，仍应从当前 CMakeCache 核对；MSVC 2022 x64、动态 Qt、Release。Linux 现有验证环境为 WSL Ubuntu 24.04、Qt 6.4.2、GCC 13。路径和实际可用性以执行时探测为准。

## 8. 验收：全部通过才交付

### 包与依赖

- v1 普通插件仍可加载；v2 空依赖插件、媒体依赖插件及 Linux 服务端插件正确读取。
- 元信息、文件内容、块顺序篡改；截断/尾随；路径穿越/链接/大小写重复；超限；错误 API/IID/平台/Qt 构建；缺失传递依赖，都在业务入口执行前失败。
- 同进程两包共享相同运行依赖只激活一份；同名不同哈希明确拒绝；两个宿主进程各自运行，不互删缓存。
- 包内不存在宿主基础 Qt/系统 DLL；许可证完整且可导出；实际 media/FFmpeg 文件从包内目录解析。
- 未重新链接业务 DLL 时修改运行文件或 helper，显式 package 目标仍更新文件清单及包。

### Windows 干净运行环境

- 使用全新测试运行目录，仅复制新主程序包和新 `.ztplugin`，不从旧目录迁移媒体库。
- 运行测试进程时清除 Qt SDK 的 PATH、QT_PLUGIN_PATH、QML_IMPORT_PATH 注入；测试使用的 Qt6Test.dll 只单独提供，不能让它带入完整开发 Kit。
- 不安装/不启用语音插件时，主程序可正常运行，模块列表中不出现 Qt6Multimedia/FFmpeg；主程序 ZIP 中也没有这些文件。
- 导入语音包后，实际创建页面、列出设备、录放音、调节三级音量、静音、连接/断开均通过。模块来源由进程模块列表核对，来自运行依赖私有目录。
- 测试初始化期间卸载、活动页面卸载、关闭时取消、卸载失败留句柄、禁用状态及重新加载，保留现有生命周期回归。
- 暖机后至少 20 轮加载/卸载，入口 DLL 真正卸载，线程/设备/回调/缓冲区回到基线；相同依赖的目录数和登记句柄数不增长。记录内存曲线，区分后端保留和逐轮增长；不得以第二次初始化更快作为泄漏结论。
- 修改插件业务 DLL、保持运行依赖不变，可直接重载；更换同名不兼容媒体库时明确提示重启，重启后加载新包。
- 用有/无语音构建缓存两种状态生成主程序，验证媒体依赖都不进入主程序包。
- 启动后清理上一次已退出进程的缓存；活动进程缓存不清理。进程结束时不因后端析构访问已卸载库而崩溃。

### Linux 与独立构建

- Linux 验证工具写读 v2、多文件校验、旧包兼容，以及服务端真实加载语音包和既有语音服务端集成测试。
- 将 SDK 解压到仓库外，独立构建示例插件；不得依赖宿主源码、主程序 dist 或仓库构建缓存。
- 首次更新完主程序/SDK后，仅运行插件独立脚本，验证主程序 EXE、ZIP、目录及 SDK ZIP 的哈希均未变化，只有插件产物更新。
- 核对 ZIP/目录内容、版本、许可证和 SDK 指纹；Windows 完成实际加载与录放音验证。若缺可用设备或某项实机条件，明确记录未完成项，不能以模拟音频代替实机通过。

## 9. 可直接转交的任务说明

> 请按 `docs/plugin-runtime-separation-plan.md` 实施媒体运行依赖与主程序部署分离，完成通用 v2 插件封装、宿主运行依赖管理、SDK 工具、语音包与独立构建入口，以及文档和验收。保留当前工作树未提交改动；VERSION/API/IID 不变。主程序及 SDK首次更新后，语音插件能够只靠安装后的 SDK 独立编译，并通过单个 `.ztplugin` 携带媒体依赖。不得把 Qt Multimedia/FFmpeg重新部署到主程序目录，也不得在热卸载时误释放应用级媒体后端。完成后报告实际产物、验证结果、文件数量/大小与内存观察；未完成的平台或实机验证明确列出。
