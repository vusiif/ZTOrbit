# ZTGUI

基于 C++17、Qt6 Quick/QML、动态 Qt 和 CMake 的 ZeroTier 客户端与插件化 Linux 服务端。

## 项目概述

客户端使用 QML 界面与 C++ 异步控制器／增量模型，负责 ZeroTier 管理，插件提供菜单与页面；服务端负责 ZeroTier 只读查询和 TCP/UDP 插件路由。两端各有独立公开 API SDK，共用版本来源与传输协议。

客户端：Windows x64 已验证；Apple Silicon macOS 待实机验证。服务端：Linux/root，已验证 Ubuntu24.04 x86_64，Qt6.4 Core/Network，不依赖界面或音频库。

部署假设与验证范围见 [维护约定](docs/maintenance.md)。

设置中的“禁止GPU加速”默认关闭，开启后使用CPU绘制，完全退出并重新启动后生效；`ZTGUI.exe --disable-gpu-acceleration` 可仅对本次启动强制禁用GPU加速。占用实测、适用范围与打包选项见 [资源占用分析](docs/resources.md)。

```text
src/client/api/       公共插件 API SDK
src/client/main/      主程序与插件注册管理
src/client/assets/    图标与资源
src/server/api/       服务端 SDK（ZTServer::Api）
src/server/main/      无界面宿主与内嵌安装脚本
src/plugins/          按项目整合插件源码及文档
  voice-chat/client/  语音客户端插件
  voice-chat/server/  语音服务端插件
  voice-chat/shared/  两端共享的语音协议
src/shared/include/   公共工具与路由编解码
src/tests/            回归测试
scripts/              构建脚本
docs/                 详细文档
.agent/skills/        项目内开发 SKILL
VERSION               开发者维护的程序版本，API 版本取前两段
```

## 编译

需要 Qt 6.8+、CMake 3.21.1+ 和 C++17 编译器。Windows 推荐 MSVC 2022，在项目根目录执行（替换 Qt SDK 路径）：

```powershell
$qtSdk = 'C:/Qt/6.8.3/msvc2022_64'
cmake -S . -B cmake-build-release -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$qtSdk"
cmake --build cmake-build-release --config Release --target zerotier_gui --parallel
& "$qtSdk/bin/windeployqt.exe" --release --qmldir src/client/main/qml cmake-build-release/Release/ZTGUI.exe
```

产物位于 `cmake-build-release/Release/`。统一使用动态 Qt，不再提供静态 Qt 或单 EXE 构建。

Windows 打包：`./.agent/skills/ztgui-build/scripts/build-windows.ps1 -QtRoot $qtSdk`，生成主程序 `dist/windows-x64.zip`，API 变化时更新 SDK 压缩包（当前为 `dist/ZTGUI-api-v0.4.zip`）。默认不编译插件；明确指定语音插件时加 `-BuildVoicePlugin`。插件以 `.ztplugin` 独立放在 `dist/plugins/`，可通过客户端导入／热加载；媒体依赖随 v2 包交付。更新 SDK 后可用独立 `build-plugin-windows.ps1` 构建插件。完整流程见 [编译与打包 SKILL](.agent/skills/ztgui-build/SKILL.md)。

macOS 执行 `bash scripts/build-macos.sh`，环境准备见 [macOS 构建说明](docs/macos.md)。

服务端：`cmake -S src/server -B cmake-build-server -DCMAKE_BUILD_TYPE=Release` 后构建目标 `zt-server`。`bash scripts/build-linux.sh` 打包宿主与服务端 SDK；明确需要语音插件时添加 `--build-voice-plugin`。根构建也可用 `-DZTGUI_BUILD_CLIENT=OFF -DZTGUI_BUILD_SERVER=ON`。命令行、运行依赖和部署见 [服务端说明](src/server/README.md)。

## 插件编写

在 `src/plugins/myplugin/client/` 创建独立 CMake 项目，引用公开 API 后编写菜单与 QML 页面代码，再独立编译。服务端插件放在同项目的 `server/`，两端共享业务代码放在 `shared/`。API 有两种引用方式：

- 克隆本项目，使用 `add_subdirectory` 引入 `src/client/api/`。
- 使用安装后的 SDK（如 `dist/ZTGUI-api-v0.4/`），通过 `find_package(ZTGUIPluginApi 0.4 EXACT CONFIG REQUIRED)` 引入。

两种方式均链接 `ZTGUI::Api`。动态插件通过借用的 `QQmlEngine*` 与父项创建 `QQuickItem*` 页面，实现 `<ztgui/PluginInterface.h>` 接口，使用 `Q_PLUGIN_METADATA` 导出，并携带 SDK 派生的 API 版本元数据。ZTGUI 启动时扫描可执行文件旁的 `plugins/` 并注册菜单；插件菜单支持运行时加载／卸载，卸载保留禁用状态，无需重新编译主程序。

公开插件类型位于 `vusiif::ztgui::api`，工具位于 `vusiif::ztgui::util`。可选 `PluginLifecycleInterface` 提供唯一带取消令牌的 `onCreate`、销毁及菜单通知，并注入文件、只读 ZeroTier 查询、多中转广播及同 ID 插件接收服务；插件数据位于 `plugins-data/<插件名称>/`。退出时异步等待初始化结束，期间GUI响应。客户端入口／生命周期IID为 `/3.0`，旧DLL须重编并封装为 `.ztplugin`。

语音项目位于 `src/plugins/voice-chat/`，包含 `client/`、`server/`、`shared/`。支持临时房间和多人 Opus 语音，默认 TCP/UDP18100，面板需手动连接。双方使用统一路由协议；旧独立服务协议不兼容。见 [客户端语音文档](src/plugins/voice-chat/client/docs/voice-chat.md) 与 [服务端语音文档](src/plugins/voice-chat/server/docs/voice-chat.md)。

完整步骤见 [插件开发 SKILL](.agent/skills/ztgui-plugin-dev/SKILL.md)、[客户端 API](docs/plugin-api.md) 与 [服务端 API](docs/server-api.md)。
