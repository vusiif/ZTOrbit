---
name: ztgui-plugin-dev
description: 在本项目中创建、编写、修改和独立编译 ZTGUI 的 C++/Qt 插件，使用公开 API SDK 定义菜单与页面。适用于插件开发，不用于普通主程序界面修改。
---

# ZTGUI 插件开发

## 先确认项目接口

从本文件所在目录向上三级是仓库根目录。读取根目录 `AGENTS.md`、`VERSION`、[插件 API 文档](../../../docs/plugin-api.md) 和 [公开接口](../../../src/api/include/ztgui/PluginApi.h)，确认实际 API 与接入能力。

应用版本为 `0.<核心版本>.<后缀版本>`，API 版本为前两段。当前 API 为 `0.2`；创建插件时读取实际 `VERSION`，据此填写 CMake 的 API 版本要求。非核心更新只递增应用后缀并维护 `CHANGELOG.md`，API 保持不变。同一任务内不要重复递增版本。

## 创建插件目录

默认在 `src/plugins/` 下创建独立插件目录，例如 `src/plugins/myplugin/`。如果用户明确指定 `src/myplugin/`，使用指定路径，后续引用 API 和独立编译的流程相同。修改已有插件时复用它的目录。

```text
src/plugins/myplugin/
├── CMakeLists.txt
├── Plugin.h
└── Plugin.cpp
```

插件只引用公开 SDK 和必要的第三方依赖。宿主的注册表、窗口及 ZeroTier 业务实现位于 `src/main/`，不要让插件包含这些内部头文件。需要新的宿主能力时先明确 API 设计，不通过访问宿主内部对象绕过接口。

## 引用 API 并编写代码

下面是可独立编译的静态库示例。替换项目/目标名、命名空间、插件 ID、标题和页面实现，并使用从 `VERSION` 读取的 API 版本。

### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.21.1)
project(myplugin LANGUAGES CXX)
set(CMAKE_AUTOMOC ON)

find_package(ZTGUIPluginApi 0.2 EXACT CONFIG REQUIRED)
add_library(myplugin STATIC Plugin.cpp Plugin.h)
target_link_libraries(myplugin PUBLIC ZTGUI::Api)
if(MSVC)
    target_compile_options(myplugin PRIVATE /utf-8)
endif()
```

`ZTGUI::Api` 提供公开头文件路径、C++17 要求及 Qt Widgets 依赖。示例公开头文件暴露 API 类型，因此用 `PUBLIC` 向引用插件目标的宿主传播依赖；只在内部实现使用 SDK 时可改为 `PRIVATE`。

### Plugin.h

```cpp
#pragma once
#include <ztgui/PluginApi.h>

namespace MyPlugin {
ZtGui::Plugins::PluginDefinition definition();
}
```

### Plugin.cpp

```cpp
#include "Plugin.h"
#include <QLabel>

ZtGui::Plugins::PluginDefinition MyPlugin::definition()
{
    using namespace ZtGui::Plugins;
    PluginDefinition plugin;
    plugin.info = {"example.myplugin", QStringLiteral("我的插件"), "0.1.0"};

    MenuContribution menu;
    menu.id = "overview";
    menu.title = QStringLiteral("我的插件");
    menu.createPage = [](QWidget *parent) -> QWidget * {
        return new QLabel(QStringLiteral("插件页面"), parent);
    };
    plugin.menus.append(menu);
    return plugin;
}
```

插件自身的版本字符串独立于宿主版本。保留 `PluginInfo` 默认的 API 版本字段，不自行定义另一份 `ApiMajor`、`ApiMinor` 或生成的 `ApiVersion.h`。

插件 ID 和菜单 ID 使用 `[a-z0-9][a-z0-9._-]{0,127}`，插件 ID 不得为 `builtin`。页面工厂在 GUI 线程执行，返回由传入 `parent` 持有的新控件；同一控件不能被多个菜单复用。耗时工作异步执行，回调绑定 QObject 上下文或使用 QPointer。页面与工厂资源必须先于插件代码卸载被释放，完整约定以 API 文档为准。

## 安装 SDK 与独立编译

在仓库根目录运行以下 PowerShell 示例。`$qtSdk` 替换为实际动态 Qt SDK 路径；优先复用项目已有的匹配 SDK，不将本机绝对路径写进插件源码。主程序和插件需使用匹配的 Qt、编译器、架构、运行库和构建模式。

```powershell
$qtSdk = 'C:/Qt/6.8.3/msvc2022_64'
$apiVersion = ((Get-Content VERSION -Raw).Trim().Split('.')[0..1] -join '.')
$apiSdk = Join-Path (Get-Location) "dist/ZTGUI-api-v$apiVersion"

cmake -S src/api -B cmake-build-api -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$qtSdk"
cmake --install cmake-build-api --prefix "$apiSdk"

cmake -S src/plugins/myplugin -B cmake-build-plugin-myplugin -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$apiSdk;$qtSdk"
cmake --build cmake-build-plugin-myplugin --config Release --parallel
```

实际执行时，每一步检查退出码，失败就先修复当前步骤，不能继续使用旧产物。若插件目录是 `src/myplugin/`，相应修改 `-S` 参数。此示例生成 `cmake-build-plugin-myplugin/Release/myplugin.lib`；静态 Qt 构建需另外匹配静态运行库配置。

## 验证与接入

确认独立目标构建成功、产物存在、源码只依赖公开 SDK。根据插件实际行为验证页面创建、父控件所有权和资源清理；不因新增目录或说明文档而重复运行完整 GUI 测试。

当前 API 提供进程内 `PluginDefinition` 注册，还没有统一的外部 DLL 入口协议和自动加载器。上述静态库是可编译的插件实现，构建成功不代表已出现在主程序菜单中。仅当任务要求接入宿主时，才由宿主侧链接插件，并在 GUI 线程调用其 `definition()`，交给宿主 `PluginRegistry::registerPlugin()` 注册。

若用户明确要求生成动态库，可改用 `MODULE` 目标并针对实际加载器设计入口；当前不能把 `PluginDefinition` 直接宣称为稳定的跨 DLL ABI，也不能宣称把 DLL 复制到目录就会自动启用。生成 DLL 与实现加载机制的范围分别以用户请求为准。

交付时说明插件源码目录、API 版本、实际构建命令、产物位置，以及是否已接入宿主。
