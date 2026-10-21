# ZTGUI

基于 C++17、Qt6 Quick/QML、动态 Qt 和 CMake 的 ZeroTier 客户端，以及插件化 Linux 服务端（Java 25 + Netty + Maven）。

## 项目概述

客户端使用 QML 界面与 C++ 异步控制器／增量模型，负责 ZeroTier 管理，插件提供菜单与页面；服务端负责 ZeroTier 只读查询、Java 插件路由和网络中转。两端各有独立公开 API SDK，共用版本来源与传输协议。

客户端：Windows x64 已验证；Apple Silicon macOS 待实机验证。服务端：Linux/root，已验证 Ubuntu 24.04 x86_64，并在 Ubuntu 22.04 容器验证同一 fat jar；需要 JDK 25，不依赖界面或音频库。

部署假设与验证范围见 [维护约定](docs/maintenance.md)。

设置中的“禁止GPU加速”默认开启，使用CPU绘制；首次切换时弹窗提醒完全退出并重新启动后生效，已有显式设置继续保留。`ZTGUI.exe --disable-gpu-acceleration` 可仅对本次启动强制禁用GPU加速。0.3.2 的历史占用测量与适用范围见 [资源占用分析](docs/validation/resources-2026-10-13/README.md)。

```text
src/client/api/       公共插件 API SDK
src/client/main/      主程序与插件注册管理
src/client/assets/    图标与资源
src/server/api/       服务端 Java 插件 SDK（Maven 模块 ztserver-api）
src/server/main/      Java 25 + Netty 宿主（Maven 模块 ztserver）
src/server/scripts/   start-server.sh、systemd unit、独立 zt-installer.sh
src/server-old/       归档的旧 C++/Qt6 服务端、测试与 C++ 语音插件
src/plugins/          按项目整合插件源码及文档
  media/client/       独立媒体工作进程与可安装的媒体 API SDK
  voice-chat/client/  语音界面、房间及网络业务插件
  voice-chat/server/  Java 语音服务端插件
  voice-chat/shared/  客户端与归档 C++ 服务端共用的语音协议（C++ Protocol.h）
src/shared/include/   客户端/旧插件共用的公开工具与路由编解码
src/tests/            回归测试
scripts/              构建脚本
docs/                 详细文档
.agents/skills/       项目内开发 SKILL
VERSION               客户端程序版本；客户端 API 取前两段数字和同一后缀
SERVER_VERSION        客户端指定的服务端程序版本；服务端 API 同规则派生
```

## 编译

需要 Qt 6.8+、CMake 3.21.1+ 和 C++17 编译器。Windows 推荐 MSVC 2022，在项目根目录执行（替换 Qt SDK 路径）：

```powershell
$qtSdk = 'C:/Qt/6.8.3/msvc2022_64'
cmake -S . -B build/cmake-build-release -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$qtSdk"
cmake --build build/cmake-build-release --config Release --target zerotier_gui --parallel
& "$qtSdk/bin/windeployqt.exe" --release --qmldir src/client/main/qml build/cmake-build-release/Release/ZTGUI.exe
```

产物位于 `build/cmake-build-release/Release/`。统一使用动态 Qt，不再提供静态 Qt 或单 EXE 构建。

Windows 打包：`./.agents/skills/ztgui-build/scripts/build-windows.ps1 -QtRoot $qtSdk`，生成主程序 `build/dist/windows-x64.zip`，API 变化时更新 SDK 压缩包（当前为 `build/dist/ZTGUI-api-v0.5.zip`）。默认不编译插件；明确指定媒体与语音插件时加 `-BuildMediaPlugin -BuildVoicePlugin`。插件以 `.ztplugin` 独立放在 `build/dist/plugins/`，可通过客户端导入／热加载；媒体依赖随独立媒体服务的 v3 包交付；语音同时引用安装后的媒体 API SDK。更新 SDK 后可用独立 `build-plugin-windows.ps1` 构建插件。完整流程见 [编译与打包 SKILL](.agents/skills/ztgui-build/SKILL.md)。

macOS 执行 `bash scripts/build-macos.sh`，环境准备见 [macOS 构建说明](docs/macos.md)。

服务端版本由根 `SERVER_VERSION` 指定（当前 `0.2.1`），与客户端 `VERSION`（当前 `0.5.1`）独立。服务端需要 Linux、JDK 25 与 Maven 3.9+。构建：

```bash
revision="$(tr -d '\r\n' < SERVER_VERSION)"
api_version="$(printf '%s' "$revision" | sed -E 's/^0\.([0-9]+)\.[0-9]+(-[0-9A-Za-z.-]+)?$/0.\1\2/')"
mvn -f src/server/pom.xml -Drevision="$revision" -DapiVersion="$api_version" clean verify
bash scripts/build-linux.sh --build-voice-plugin   # 可选：同时构建 Java 语音插件
```

`bash scripts/build-linux.sh` 生成 `build/dist/linux-x64.tar.gz` 与服务端 SDK `build/dist/ZTServer-api-v0.2.zip`；语音插件独立发布到 `build/dist/server-plugins/`。旧 C++ 服务端只可显式构建：`-DZTGUI_BUILD_CLIENT=OFF -DZTGUI_BUILD_SERVER_OLD=ON`，见 [归档说明](src/server-old/README.md)。命令行、插件、运行依赖和部署见 [服务端说明](src/server/README.md)。

## 插件编写

在 `src/plugins/myplugin/client/` 创建独立 CMake 项目，引用公开 API 后编写菜单与 QML 页面代码，再独立编译。服务端 Java 插件放在同项目的 `server/`；客户端与归档 C++ 服务端共用的 C++ 业务代码放在 `shared/`（Java 服务端按线协议重新实现）。客户端 API 有两种引用方式：

- 克隆本项目，使用 `add_subdirectory` 引入 `src/client/api/`。
- 使用安装后的 SDK（如 `build/dist/ZTGUI-api-v0.5/`），通过 `find_package(ZTGUIPluginApi 0.5 EXACT CONFIG REQUIRED)` 引入；包元数据中的完整 API 为 `0.5`。

两种方式均链接 `ZTGUI::Api`。动态插件通过借用的 `QQmlEngine*` 与父项创建 `QQuickItem*` 页面，实现 `<ztgui/PluginInterface.h>` 接口，使用 `Q_PLUGIN_METADATA` 导出，并携带 SDK 派生的 API 版本元数据。ZTGUI 启动时扫描可执行文件旁的 `plugins/` 并注册菜单；插件菜单支持运行时加载／卸载，卸载保留禁用状态，无需重新编译主程序。

公开插件类型位于 `vusiif::ztgui::api`，工具位于 `vusiif::ztgui::util`。可选 `PluginLifecycleInterface` 提供唯一带取消令牌的 `onCreate`、销毁及菜单通知，并注入文件、只读 ZeroTier 查询、多中转广播及同 ID 插件接收服务；插件数据按稳定 ID 位于 `plugins-data/id-<插件ID>/`，旧显示名目录由宿主歧义感知迁移，宿主设置、日志和缓存使用可回退到用户目录的活动存储根。可选 `PluginStopInterface/1.0` 支持业务异步停止；`ServiceAccess` 提供清单限定的服务租约。加载先认证并解析完整依赖图，卸载先结束消费者再结束提供者，期间 GUI 保持响应。详见[服务与隔离契约](docs/plugin-services.md)。客户端入口／生命周期IID为 `/3.0`，旧DLL须重编并封装为 `.ztplugin`。

语音项目位于 `src/plugins/voice-chat/`，包含 `client/`、`server/`、`shared/`。支持临时房间和多人 Opus 语音，默认 TCP/UDP18100，面板需手动连接。双方使用统一路由协议；旧独立服务协议不兼容。见 [客户端语音文档](src/plugins/voice-chat/client/docs/voice-chat.md) 与 [服务端语音文档](src/plugins/voice-chat/server/docs/voice-chat.md)。

完整步骤见 [插件开发 SKILL](.agents/skills/ztgui-plugin-dev/SKILL.md)、[客户端 API](docs/plugin-api.md) 与 [服务端 API](docs/server-api.md)。服务端 Java 插件使用 [ZTServer SDK](docs/server-api.md)，旧 C++ `.ztplugin` 仅存在于 `src/server-old/`。
