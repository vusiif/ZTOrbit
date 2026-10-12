# ZTGUI

基于 C++17、Qt 6 Widgets 和 CMake 的 ZeroTier 管理工具。

## 项目概述

主程序负责界面和 ZeroTier 管理，公开 API SDK 供插件独立编译，插件通过 API 提供菜单和页面。

操作系统：Windows x64 已验证；Apple Silicon macOS 已提供实现和构建脚本，待实机验证。

部署假设与验证范围见 [维护约定](docs/maintenance.md)。

```text
src/api/       公共插件 API SDK
src/main/      主程序与插件注册管理
src/plugins/   插件源码及各自文档，包含 voice-chat/
src/assets/    图标与资源
tests/         回归测试
scripts/       构建脚本
docs/          详细文档
.agent/skills/ 项目内开发 SKILL
VERSION        开发者维护的程序版本，API 版本取前两段
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

## 插件编写

在 `src/plugins/myplugin/` 创建独立 CMake 项目，引用公开 API 后编写菜单与页面代码，再独立编译。API 有两种引用方式：

- 克隆本项目，使用 `add_subdirectory` 引入 `src/api/`。
- 使用安装后的 SDK（如 `dist/ZTGUI-api-v0.3/`），通过 `find_package(ZTGUIPluginApi 0.3 EXACT CONFIG REQUIRED)` 引入。

两种方式均链接 `ZTGUI::Api`。动态插件实现 `<ztgui/PluginInterface.h>` 接口，使用 `Q_PLUGIN_METADATA` 导出，并携带 SDK 派生的 API 版本元数据。ZTGUI 启动时扫描可执行文件旁的 `plugins/` 并注册菜单；增减插件后重启即可，无需重新编译主程序。

公开插件类型位于 `vusiif::ztgui::api`，工具位于 `vusiif::ztgui::util`。可选 `PluginLifecycleInterface` 提供唯一带取消令牌的 `onCreate`、销毁及菜单通知，并注入文件与异步 ZeroTier 查询服务；插件数据位于 `plugins-data/<插件名称>/`。关闭窗口异步等待初始化结束，期间GUI响应。当前接口重构不保留旧插件兼容层，旧DLL须重编。

语音插件位于 `src/plugins/voice-chat/client-plugin/`，无界面服务端位于其同级 `server/`。支持临时房间和多人 Opus 语音，默认端口 TCP/UDP `18100`，面板需手动填写地址并点击连接。独立构建、Linux 部署和协议由插件项目维护，见 [语音插件文档](src/plugins/voice-chat/docs/voice-chat.md)。

完整步骤见 [插件开发 SKILL](.agent/skills/ztgui-plugin-dev/SKILL.md) 和 [API 文档](docs/plugin-api.md)。
