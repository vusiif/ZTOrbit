# ZTGUI

基于 C++17、Qt 6 Widgets 和 CMake 的 ZeroTier 管理工具。

## 项目概述

主程序负责界面和 ZeroTier 管理，公开 API SDK 供插件独立编译，插件通过 API 提供菜单和页面。

操作系统：Windows x64 已验证；Apple Silicon macOS 已提供实现和构建脚本，待实机验证。

部署假设与验证范围见 [维护约定](docs/maintenance.md)。

```text
src/api/       公共插件 API SDK
src/main/      主程序与插件注册管理
src/plugins/   插件源码，每个插件一个子目录
tests/         回归测试
assets/        图标与资源
scripts/       构建脚本
docs/          详细文档
.agent/skills/ 项目内开发 SKILL
VERSION        主程序版本，API 版本取前两段
```

## 编译

需要 Qt 6.8+、CMake 3.21.1+ 和 C++17 编译器。Windows 推荐 MSVC 2022，在项目根目录执行（替换 Qt SDK 路径）：

```powershell
$qtSdk = 'C:/Qt/6.8.3/msvc2022_64'
cmake -S . -B cmake-build-release -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$qtSdk"
cmake --build cmake-build-release --config Release --parallel
& "$qtSdk/bin/windeployqt.exe" --release cmake-build-release/Release/ZTGUI.exe
```

产物位于 `cmake-build-release/Release/`。单 EXE 版本使用静态 Qt SDK 构建，无需运行 `windeployqt`。

macOS 执行 `bash scripts/build-macos.sh`，环境准备见 [macOS 构建说明](docs/macos.md)。

## 插件编写

在 `src/plugins/myplugin/` 创建独立 CMake 项目，引用公开 API 后编写菜单与页面代码，再独立编译。API 有两种引用方式：

- 克隆本项目，使用 `add_subdirectory` 引入 `src/api/`。
- 使用安装后的 SDK（如 `dist/ZTGUI-api-v0.2/`），通过 `find_package(ZTGUIPluginApi 0.2 EXACT CONFIG REQUIRED)` 引入。

两种方式均链接 `ZTGUI::Api`，代码使用 `#include <ztgui/PluginApi.h>`。当前 API 支持进程内注册，外部 DLL 自动加载尚未实现。

完整步骤见 [插件开发 SKILL](.agent/skills/ztgui-plugin-dev/SKILL.md) 和 [API 文档](docs/plugin-api.md)。
