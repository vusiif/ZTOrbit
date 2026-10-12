# ZTGUI

基于 C++17、动态 Qt6 和 CMake 的 ZeroTier 客户端与插件化 Linux 服务端。

## 项目概述

客户端负责界面和 ZeroTier 管理，插件提供菜单与页面；服务端负责 ZeroTier 只读查询和 TCP/UDP 插件路由。两端各有独立公开 API SDK，共用版本来源与传输协议。

客户端：Windows x64 已验证；Apple Silicon macOS 待实机验证。服务端：Linux/root，已验证 Ubuntu24.04 x86_64，Qt6.4 Core/Network，不依赖界面或音频库。

部署假设与验证范围见 [维护约定](docs/maintenance.md)。

```text
src/client/api/       公共插件 API SDK
src/client/main/      主程序与插件注册管理
src/client/plugins/   插件源码及各自文档，包含 voice-chat/
src/client/assets/    图标与资源
src/server/api/       服务端 SDK（ZTServer::Api）
src/server/main/      无界面宿主与内嵌安装脚本
src/server/plugins/   服务端插件及各自文档
src/shared/           公共工具、路由编解码与语音协议
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
& "$qtSdk/bin/windeployqt.exe" --release cmake-build-release/Release/ZTGUI.exe
```

产物位于 `cmake-build-release/Release/`。统一使用动态 Qt，不再提供静态 Qt 或单 EXE 构建。

Windows 打包：`./.agent/skills/ztgui-build/scripts/build-windows.ps1 -QtRoot $qtSdk`，生成主程序 `dist/windows-x64.zip`，API 变化时更新 SDK 压缩包（当前为 `dist/ZTGUI-api-v0.3.zip`）。默认不编译插件；明确指定语音插件时加 `-BuildVoicePlugin`。插件单独放在 `dist/plugins/`，使用时复制 DLL 到 `ZTGUI.exe` 旁的 `plugins/` 后重启。完整流程见 [编译与打包 SKILL](.agent/skills/ztgui-build/SKILL.md)。

macOS 执行 `bash scripts/build-macos.sh`，环境准备见 [macOS 构建说明](docs/macos.md)。

服务端：`cmake -S src/server -B cmake-build-server -DCMAKE_BUILD_TYPE=Release` 后构建目标 `zt-server`。`bash scripts/build-linux.sh` 打包宿主与服务端 SDK；明确需要语音插件时添加 `--build-voice-plugin`。根构建也可用 `-DZTGUI_BUILD_CLIENT=OFF -DZTGUI_BUILD_SERVER=ON`。命令行、运行依赖和部署见 [服务端说明](src/server/README.md)。

## 插件编写

在 `src/client/plugins/myplugin/` 创建独立 CMake 项目，引用公开 API 后编写菜单与页面代码，再独立编译。API 有两种引用方式：

- 克隆本项目，使用 `add_subdirectory` 引入 `src/client/api/`。
- 使用安装后的 SDK（如 `dist/ZTGUI-api-v0.3/`），通过 `find_package(ZTGUIPluginApi 0.3 EXACT CONFIG REQUIRED)` 引入。

两种方式均链接 `ZTGUI::Api`。动态插件实现 `<ztgui/PluginInterface.h>` 接口，使用 `Q_PLUGIN_METADATA` 导出，并携带 SDK 派生的 API 版本元数据。ZTGUI 启动时扫描可执行文件旁的 `plugins/` 并注册菜单；增减插件后重启即可，无需重新编译主程序。

公开插件类型位于 `vusiif::ztgui::api`，工具位于 `vusiif::ztgui::util`。可选 `PluginLifecycleInterface` 提供唯一带取消令牌的 `onCreate`、销毁及菜单通知，并注入文件、异步 ZeroTier 查询与网络数据发送服务；插件数据位于 `plugins-data/<插件名称>/`。退出时异步等待初始化结束，期间GUI响应。当前接口重构不保留旧插件兼容层，旧DLL须重编。

语音客户端位于 `src/client/plugins/voice-chat/client-plugin/`，服务端插件位于 `src/server/plugins/voice-chat/`。支持临时房间和多人 Opus 语音，默认 TCP/UDP18100，面板需手动连接。双方使用统一路由协议；旧独立服务协议不兼容。见 [客户端语音文档](src/client/plugins/voice-chat/docs/voice-chat.md) 与 [服务端语音文档](src/server/plugins/voice-chat/docs/voice-chat.md)。

完整步骤见 [插件开发 SKILL](.agent/skills/ztgui-plugin-dev/SKILL.md)、[客户端 API](docs/plugin-api.md) 与 [服务端 API](docs/server-api.md)。
