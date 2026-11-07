---
name: zto-build
description: 为 ZTOrbit 编译、打包和清理构建产物，按 API 变化更新 SDK；用户指定或插件自身版本变化时重编对应插件，不保留构建备份。
---

# ZTOrbit 编译与打包

## 默认范围

1. 编译主程序并部署运行依赖，压缩主程序文件夹。
2. API 有变化时独立配置、安装 SDK 并压缩；没有变化则复用已有 SDK 和压缩包。
3. 用户明确要求或插件自身版本号发生变化时，重新编译并打包对应插件。其余仅修改插件源码、宿主 API 或发现旧 DLL 的情况，不自动扩大插件构建范围。

先定位当前 ZTOrbit 工作树根目录，读取根 `AGENTS.md`、`VERSION`（客户端）和 `SERVER_VERSION`（服务端，由客户端侧指定）。不要从全局技能安装路径推断仓库位置。版本由开发者维护，编译与打包不能自行更改版本文件；客户端与服务端版本独立，各端 API 取本端版本号前两段数字并保留相同预发布后缀，应用补丁变化不触发 API 更新。

构建缓存、工具链依赖和发行产物统一写入 `build/`（`build/cmake-build-*`、`build/dist/`）；仓库根目录不应出现 `cmake-build-*` 或 `dist/`。迁移前遗留的根目录 CMake 缓存不能直接复用，应让其在新路径重新配置。

## Windows 入口

当前已验证的交付目标是 Windows x64、MSVC 2022、动态 Qt 6.8+。优先使用用户提供的 Qt SDK；未提供时从现有 CMakeCache 或本机 SDK 定位，不写死某一开发者的路径。

在仓库根目录执行本技能的脚本：

```powershell
$qtSdk = 'C:/Qt/6.8.3/msvc2022_64' # 替换为实际动态 SDK
& ./.agents/skills/zto-build/scripts/build-windows.ps1 -QtRoot $qtSdk
```

[build-windows.ps1](scripts/build-windows.ps1) 控制主程序、条件 API 更新与可选插件；[PackageTools.ps1](scripts/PackageTools.ps1) 提供路径检查、内容指纹、干净目录替换和 ZIP 创建。仓库 `scripts/build-windows.ps1` 仅为兼容入口，遵守相同的默认范围。

Windows默认运行包只保留宿主使用的Basic控件样式、中文／英文Qt翻译和运行插件，不带QML调试工具；媒体运行依赖随独立工作进程 v3 包交付，主程序不读取业务 DLL。Qt Quick默认使用CPU绘制，用户可在设置中启用GPU加速，因此不默认部署Mesa的 `opengl32sw.dll`。需要显式OpenGL软件兼容环境时添加 `-IncludeOpenGLSoftware`；第三方插件明确导入其他控件样式时添加 `-KeepAllQmlStyles`。裁剪仅发生在已校验的干净暂存目录，不能处理用户已有的插件／数据目录。

脚本用显式目标 `zto` 构建主程序。不要用默认“构建全部目标”替代，以免现有构建缓存启用了测试或插件。GUI 回归、服务端、语音面板和音频测试不是普通打包的默认构建目标；根据用户要求或实际代码变化选择需要的验证。

## 自动更新 ZIP

将干净的 `build/dist/Z2-windows-x64.zip` 上传到服务器选定位置，在 `/root/Server/Z2Updater/` 运行 `bash ./update.sh --version <VERSION> --file <ZIP路径>`。`--file` 必填，支持绝对路径或相对执行时工作目录的路径，不约定固定文件或上传目录。本技能的 `scripts/publish-update.py` 部署为服务器 `tools/publish-update.py`，负责读取包内 PE 版本、转换 ZIP 外目录、生成清单并用服务器 root 专用私钥签名、校验和切换发布；同版本不同内容拒绝覆盖。发布入口源文件是仓库 `scripts/update-server/update.sh`。当前部署与发布步骤见 [更新服务器](../../../docs/update-server.md)。发布已有 ZIP 不要求重新编译主程序、服务端、SDK 或插件。

## API 是否更新

API 是头文件 SDK，另附本机封装命令工具，生成过程是独立 CMake 配置、安装公开头文件/版本头/CMake 导出，再压缩，无须构造额外 API DLL；安装前构建 ztplugin_pack，随 SDK 部署工具运行依赖。

脚本将 `src/client/api/` 的文件名及内容哈希、API 代际、安装目录内容和 ZIP 哈希记录在 `build/cmake-build-api/package-state.json`，对比的是上次成功的 API 打包，不是仅看 Git 是否有未提交改动。

- 源码或 API 代际改变、SDK 缺失/内容异常、首次没有可靠记录时，生成一次干净 SDK 并更新记录。
- SDK 正常但 ZIP 缺失或损坏时，仅重新压缩 SDK。
- API 与产物均未变时，不独立配置/安装 API，不改写其 ZIP；主程序自身的 CMake 配置仍会生成其需要的 API 版本头。
- 用户明确要求重新生成 API 时，加 `-RebuildApi`。

状态记录留在构建缓存中，不进入 SDK 压缩包。API 更新时使用干净安装目录，避免已删除头文件残留。

客户端指纹同时覆盖 `src/shared/include/` 和封装工具输入，因为 SDK 安装这些公开工具与路由协议。

## Linux 服务端（Java 25 + Netty）

执行本技能的 `scripts/build-linux.sh`（或根 `scripts/build-linux.sh` 兼容入口）。脚本要求 Linux、JDK 25 与 Maven 3.9+，读取根 `SERVER_VERSION`，以 `-Drevision=<服务端版本> -DapiVersion=<前两段数字+后缀>` 执行 `mvn -f src/server/pom.xml clean verify`，生成 `build/dist/Z2Hub-linux-x64/` 与 `build/dist/Z2Hub-linux-x64.tar.gz`。包内为 `Z2Hub.jar`、`start-server.sh`、`z2hub.service`、`z2-installer.sh`、`DEPLOYMENT.md` 副本、`VERSION`（服务端版本）、`licenses/`；不内置 JRE，不携带插件、测试或运行数据。脚本在原生临时目录暂存后压缩，避免 WSL drvfs 忽略 chmod 导致 tar 权限错误。

启动脚本只检查 Java 25 与 JAR 是否存在；`--check-deps` 仅检查，失败返回 78 并给出 Ubuntu 22.04/24.04 的 Temurin 25 安装指引；无参数默认 `--server --listen 0.0.0.0 --port 18100`，其他参数原样转发，`exec java -jar` 保留信号与退出码。宿主不再包含安装/卸载/Moon 参数，独立脚本 `z2-installer.sh` 随包发布并使用原 `--action` 映射。

`scripts/package-server-sdk.py` 从 Maven 构建的 `ztohub-api` jar 生成 `build/dist/Z2Hub-api-v<API版本>/` 与 ZIP（jar/sources/javadoc/POM、sample、sdk.json），仅当 API 源码、POM 或 `SERVER_VERSION` 指纹变化时重建，ZIP 缺失/损坏时重新压缩。验收时在解压副本检查 `--version`=`SERVER_VERSION`、`--check-deps`、实际监听与 SIGTERM 退出，核对压缩包脚本权限和 SDK sample 的独立编译。

用户明确指定语音服务端插件或该插件自身版本变化时添加 `--build-voice-plugin`：先安装 API 到本地 Maven 仓库，再构建 Java 语音插件，独立发布到 `build/dist/server-plugins/z2hub_voice_chat-v0.1.0.jar`，不放入宿主包。目标机需要 JDK 25；不再需要 Qt6 或 libsodium。测试与运行数据位于 `src/tests/server/` 与 `src/tests/voice-chat/`，不进入发行包。

旧 C++ 服务端归档在 `src/server-old/`，只通过 `ZTO_BUILD_SERVER_OLD=ON` 或 `src/server-old/scripts/build-linux.sh` 显式构建；归档版本固定为 `0.4.1`／API `0.4`，输出 `build/dist/linux-x64-cpp*` 与 `build/dist/ZTOrbitHub-api-cpp-v0.4.zip`。

## 插件构建触发条件

构建用户要求的插件，以及自身版本号发生变化的插件；先读项目 `.agents/skills/zto-plugin-dev/SKILL.md`。插件自身版本以 `PluginInfo.version`、插件项目及包清单声明为准；与上次交付版本不同，或本次任务修改了该版本号，就必须重编业务二进制并重新生成 `.z2plugin`，核对二进制与包元数据的版本一致。即使只改版本号、业务源码未变，也不能复用旧二进制或仅修改包元数据；不再以“用户未显式要求编译插件”为由跳过。只构建对应客户端／服务端插件，不联动其他未变化的插件；不要为了触发构建自行修改版本号。

已有宿主 SDK 和媒体 SDK 时，单独重编语音客户端：

```powershell
& ./.agents/skills/zto-build/scripts/build-plugin-windows.ps1 `
    -PluginDirectory src/plugins/voice-chat/client -Target zto_voice_chat `
    -QtRoot $qtSdk -ApiSdk build/dist/Z2-api-v0.5 -AdditionalSdk build/dist/Z2-media-api-v2
```

其他插件按其独立项目和公开 SDK 构建，不把所有插件一起编译。正式发布文件名从包声明读取版本号，中间构建包保留固定名称。代码内的 `zto_`／`ztmedia_` 目标不变，分发文件前缀使用 `z2_`／`z2media_`。插件文件平铺到 `build/dist/plugins/`，如 `z2_voice_chat-v0.3.0.z2plugin`，可读许可证导出到对应 `z2_voice_chat-licenses/` 目录，包内也保留许可证。

主程序部署完全不读取业务插件 DLL 的依赖信息。插件独立入口仅分析本次目标，并将新增依赖封装在自己的包中。API 代际或工具链变化可能使旧 DLL 失效；自身版本没有变化且用户未指定的插件，应报告需要重编，由用户决定额外构建范围。

## 产物与完成条件

- 主程序：`build/dist/Z2-windows-x64/` 和 `build/dist/Z2-windows-x64.zip`。
- SDK：`build/dist/Z2-api-v<API版本>/` 及同名 `.zip`。
- 指定插件：`build/dist/plugins/<插件文件>`，主程序压缩包不带应用插件。

主程序只部署自身基础依赖和 libsodium，windeployqt 输入只包含主程序。运行包同时包含自动更新助手 `z2_updater.exe`（纯 Win32，无 Qt 依赖，必须与 `Z2.exe` 同级，缺失时打包脚本失败），更新协议见 `docs/auto-update.md`。禁止读取现有业务插件缓存；Multimedia、媒体后端和 FFmpeg 由独立插件收集、封装。不得在用户运行目录批量删除媒体文件。

使用干净暂存目录，成功校验后替换发行目录；不创建或保留作为交付结果的构建备份。不把日志、插件数据、模拟 CLI 或测试可执行文件带入包。每步确认成功再使用产物，失败不能宣称新包已完成。

在构建、部署及验证阶段结束后，及时删除已无用途的临时构建产物、临时脚本和截图。主程序目录、SDK、插件包、许可证目录及过期构建目录生成的 `.previous-*` 等备份也必须及时删除，包括已确认归属本项目的历史备份；无需保留“上一版构建备份”。若现有脚本为替换事务临时生成了旧目录或旧文件，在新产物校验通过后立即清理，不能因脚本默认保留备份而跳过。发布失败时先恢复有效的正式产物，再清理临时事务文件。

清理前核对文件归属和绝对路径范围，保留正式交付产物、仍使用的依赖、可复用构建缓存和已归档的验证证据；不要把仍被构建引用的第三方依赖误当临时文件删除。完成时核对本轮备份已清理，受工具策略阻止而未删除的必须如实说明。

一键清理入口为仓库根 `scripts/clean-build-temp.ps1`，实现位于本技能 `scripts/clean-build-temp.ps1`：

```powershell
./scripts/clean-build-temp.ps1                 # 删除 *.previous-* 备份并清空 .tmp 内容
./scripts/clean-build-temp.ps1 -Exclude relay-deps,screenshots/keep-*.png
./scripts/clean-build-temp.ps1 -WhatIf          # 只预览，不删除
```

脚本保留 `.tmp` 目录本身；`-Exclude` 接受相对 `.tmp` 的路径（也可写成 `.tmp/<路径>`），文件夹排除会保留整棵子树，`*`／`?` 通配符可跨层级。备份扫描跳过 `.tmp` 子树、`.git` 和 `node_modules`，不跟随也不删除链接。仍被构建复用的构建缓存、正式发行产物以及 `.tmp` 之外的其他目录不在清理范围内。

核对输出目录与压缩包的文件数量/内容校验及版本信息。API 变化时按需要验证安装后 SDK 的独立引用；指定插件时验证实际加载。交付说明主程序 ZIP、API 更新或复用的原因、是否构建了指定插件。

macOS 仅在用户指定时按仓库 `docs/macos.md` 调整工具链，仍遵守上面三项范围。现有 `scripts/build-macos.sh` 只构建 macOS 主程序与应用包，不包含应用插件；语音/媒体 worker 的 macOS 交付尚未实现，未实机运行不能声称已验证。

SDK 的 `zto_package_plugin()` 默认生成 v2；POST_BUILD 和显式 `<target>_package` 都重新校验依赖并原子输出包，运行包正式入口仅 `.z2plugin`。业务 DLL／SO 是中间构建产物，依赖分析可读取它们；不把测试夹具或旧裸插件带入正式包。Windows 主程序需部署 libsodium.dll；SDK 的 bin 包含封装工具、Qt Core 与 crypto 运行依赖。Linux 构建需要 libsodium-dev，运行需要 libsodium。

Windows 离线或自动下载失败时，可传 `-SodiumRoot <官方 MSVC 包解压后的 libsodium 目录>`；该目录应包含 include 与 x64/Release/v143/dynamic。直接 CMake 使用 `-DZTO_SODIUM_ROOT=...`，两种路径都不关闭 TLS 或内容哈希校验。

## 插件运行库独立交付

首次实施 v2 时更新客户端 SDK，即使 API 数字不变。SDK 指纹包括共享包实现、全部 helper/收集脚本和契约输入；安装的 `sdk-tools.json` 记录格式支持和内容哈希。主程序输出必须不含 Multimedia、FFmpeg、multimedia 后端及业务包，脚本在发布前检查。

已安装 SDK 可用时，执行 `./.agents/skills/zto-build/scripts/build-plugin-windows.ps1 -PluginDirectory src/plugins/voice-chat/client -Target zto_voice_chat -QtRoot $qtSdk -ApiSdk build/dist/Z2-api-v0.5 -AdditionalSdk build/dist/Z2-media-api-v2`（语音必须带 `-AdditionalSdk`，通用示例见上文“插件构建触发条件”）。脚本不构建主程序，拒绝过期 SDK，每次调用业务目标与 package 目标，原子发布 `.z2plugin` 并导出许可证。`-BuildVoicePlugin` 仅作为组合入口调用它。Kit 必须包含实际分发许可证与 FFmpeg 二进制哈希出处；缺失立即失败。详情见[运行依赖分离](../../../docs/plugin-runtime-separation.md)。

## 独立媒体与语音服务迁移

普通主程序构建中，未指定且自身版本未变的插件不编译，Release 配置明确关闭回归目标以免读取旧缓存中的插件测试路径。媒体插件被指定或自身版本变化时加 `-BuildMediaPlugin`；单独使用 `scripts/build-media-windows.ps1 -QtRoot $qtSdk -ApiSdk build/dist/Z2-api-v0.5`（脚本位于本技能 scripts/），生成 `build/dist/Z2-media-api-v2/` 及 ZIP，并构建发布 `build/dist/plugins/z2_media_worker-v2.0.0.z2plugin`。离线 Opus 可通过 `-OpusSource` 指定已验证的 1.6.1 源码。

语音需要安装后的媒体 API。独立入口增加 `-AdditionalSdk build/dist/Z2-media-api-v2`；通用额外配置用 `-CMakeArguments` 参数数组传入。`-BuildVoicePlugin` 复用该 SDK；媒体服务也被指定或自身版本变化时使用两个开关。客户端语音版本变化不自动触发服务端语音或其他插件的构建。

`SERVICE_MANIFEST` 触发 ZTPK3。worker 收集器分析 EXE，携带自己的 Core/Gui/Network、媒体后端及实际 MSVC 运行库；进程内收集器仍排除宿主基础库。两种模式不得混用排除规则。额外 Qt 模块与后端只允许工作进程；普通旧 v1/v2 插件兼容不代表旧媒体包可完全卸载。API、包格式和验证证据见[服务实现](../../../docs/plugin-services.md)。


## 通用媒体引擎与扩展 SDK

媒体包 2.0.0 提供 media.session / 1 与通用 media.clip / 1；Media API SDK 2.0.0 与 Engine SDK 1.0.0 分开交付。业务界面插件显式链接 ZTO::Api 与 ZTO::MediaApi；无界面消费者只需 Core/Network 和宿主 Services 组件。完整 FFmpeg 仅开放给 worker 侧 Engine SDK，扩展随媒体包认证清单交付。构建媒体使用 build-media-windows.ps1，可传 FfmpegRoot 复用受控依赖 SDK。API 目录及 ZIP 为 build/dist/Z2-media-api-v2，Engine SDK 为 build/dist/Z2-media-engine-sdk-v1。测试使用 run-media-tests.ps1，必须指定实际 QtRoot，避免 Qt 平台插件路径错误导致阻塞弹窗。媒体 API 或内嵌资源变化后须同时重编语音消费者，语音同时声明 session 与 clip 两个服务；详细契约由媒体项目 client/docs/media.md 维护。
