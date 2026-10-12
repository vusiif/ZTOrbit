---
name: ztgui-plugin-dev
description: 在本项目中创建、编写、修改和独立编译 ZTGUI 的 C++/Qt 动态插件，通过公开 SDK 提供菜单与页面。
---

# ZTGUI 插件开发

## 接口、目录与版本

本文件所在目录向上三级是仓库根目录。先读取根 `AGENTS.md`、`VERSION`、[接口文档](../../../docs/plugin-api.md) 和 `src/client/api/include/ztgui/` 的公开接口。插件默认位于 `src/client/plugins/<name>/`；用户指定其他目录时使用指定路径，例如语音插件的 `src/client/plugins/voice-chat/client-plugin/`。

插件只能依赖公开 SDK 和必要第三方库，不包含 `src/client/main/` 内部头文件。测试放在 `src/tests/`。需要宿主业务能力时明确设计公开服务接口，不访问宿主内部对象绕过 API。

程序版本号由开发者管理，未经开发者明确指示不得自动修改 `VERSION`。应用使用 `0.<核心>.<补丁>`，API 取 `VERSION` 前两段；开发者指定非核心新版本时仅变更补丁。未指定新版本的变更记录在 `CHANGELOG.md` 的未发布部分。插件自身版本独立于宿主；保留 `PluginInfo` 的默认 API 字段，不重复定义版本常量或生成头文件。

统一使用动态 Qt，已停止静态 Qt/单 EXE 构建。宿主与插件必须使用匹配的 Qt、编译器、架构、运行库和构建模式。`PluginDefinition` 是进程内 C++ 接口，不承诺跨工具链二进制兼容。

## 独立项目示例

目录包含 `CMakeLists.txt`、`Plugin.cpp` 和 `plugin.json.in`。这里以当前 API `0.3` 为例，实际要求应根据根 `VERSION` 填写。

```cmake
cmake_minimum_required(VERSION 3.21.1)
project(myplugin LANGUAGES CXX)
set(CMAKE_AUTOMOC ON)
find_package(ZTGUIPluginApi 0.3 EXACT CONFIG REQUIRED)
configure_file(plugin.json.in "${CMAKE_CURRENT_BINARY_DIR}/plugin.json" @ONLY)
qt_add_plugin(myplugin SHARED CLASS_NAME vusiif::ztgui::example::MyPlugin Plugin.cpp)
target_include_directories(myplugin PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(myplugin PRIVATE ZTGUI::Api)
if(MSVC)
    target_compile_options(myplugin PRIVATE /utf-8)
endif()
```

SDK 配置导出 `ZTGUI_API_VERSION`，JSON元数据必须从该值派生，不能硬编码另一份 API 版本：

```json
{"apiVersion":"@ZTGUI_API_VERSION@"}
```

```cpp
#include <ztgui/PluginInterface.h>
#include <QLabel>
namespace vusiif::ztgui::example {
class MyPlugin final : public QObject, public vusiif::ztgui::api::PluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ZTGUI_PLUGIN_IID FILE "plugin.json")
    Q_INTERFACES(vusiif::ztgui::api::PluginInterface)
public:
    vusiif::ztgui::api::PluginDefinition definition() override {
        using namespace vusiif::ztgui::api;
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
};
}
#include "Plugin.moc"
```

`ZTGUI::Api` 提供公开头文件、C++17 和 Qt Widgets 依赖。插件公共头文件若暴露 SDK 类型，链接作用域采用 `PUBLIC`；只在内部使用时采用 `PRIVATE`。

插件ID和菜单ID须匹配 `[a-z0-9][a-z0-9._-]{0,127}`，插件ID不能为`builtin`。页面工厂在GUI线程返回新建、由传入parent直接持有的控件，不能借用宿主控件或跨菜单共用页面。耗时工作放在工作线程，回调绑定QObject上下文。页面析构必须停止并等待工作线程、取消计时器/网络/音频，确保工厂和插件代码卸载前资源全部释放。

公开插件类型统一在 `vusiif::ztgui::api`，工具接口与取消类型统一在 `vusiif::ztgui::util`。插件项目自己的类型使用 `vusiif::ztgui::<模块名>`，例如语音项目的 `voice_chat`；宿主内部实现使用 `plugins/ui/zerotier/platform` 等模块。不要创建旧命名空间别名或兼容入口。

需要初始化、清理、菜单切换通知或宿主工具时，实现 `<ztgui/PluginLifecycle.h>` 的 `vusiif::ztgui::api::PluginLifecycleInterface` 并添加到 `Q_INTERFACES`。唯一初始化签名为 `api::CreateTask onCreate(util::HostServices&, const util::CancellationToken&)`。在GUI线程快速返回任务，空任务表示立即就绪；令牌按值捕获，后台任务在步骤之间检查 `isCancellationRequested()`，取消后清理部分资源并返回。任务不得访问GUI控件或承担永久业务循环，应捕获独立共享状态并保证能够结束。宿主关闭时请求取消、跳过未开始的初始化，并异步等待全部任务结束，期间GUI响应并显示待结束的插件；不能取消的第三方调用仍须等返回。

`onExitedTo(api::MenuLocation)` 先于目标插件的 `onEnteredFrom(api::MenuLocation)`，均在GUI线程执行，不得重入导航或修改注册表。`onDestroy()` 在任务完成、页面/工厂释放和查询取消后、根对象与DLL卸载前调用，应允许初始化失败或取消时资源不完整。页面仍在自身析构中同步停止线程。旧接口和单参数onCreate已移除，新IID为 `vusiif.ztgui.api.PluginInterface/1.0` 与 `vusiif.ztgui.api.PluginLifecycleInterface/1.0`，旧插件必须重编；程序版本由开发者另行管理。

保存 `onCreate()` 注入的 `HostServices` 借用指针可获得文件和ZeroTier服务，有效至 `onDestroy()` 返回。`files().readFile/writeFile` 支持后台线程，使用程序目录下的 `plugins-data/<PluginInfo.name>/`，宿主自动拼接插件名称，调用只传目录内相对路径；写入原子替换，禁止绝对路径、`..` 或链接路径。名称须合法、稳定且唯一，中文名称可用，同名插件会共用目录，改名需自行迁移数据。工具整文件读写，适用于配置和小型缓存，大文件传输应使用流式IO。`zeroTier().getZTInfo/getZTPeers/getZTNetworks/getZTMoons` 在GUI线程接受QObject上下文和异步 `JsonCallback`，Info返回JSON对象，其余返回数组，失败使用 `JsonResult::error`。上下文销毁或插件卸载时取消请求，不能在 `onDestroy()` 发起查询。详细签名和生命周期顺序见公开API文档。

## 安装 SDK、构建与启用

服务端插件使用 `src/server/plugins/<name>/` 与独立 `ZTServerPluginApi`/`ZTServer::Api`，只依赖 Qt Core/Network。先读 [服务端 API](../../../docs/server-api.md)，通过独立服务端 IID、生命周期与统一路由接入；不能引用客户端或服务端宿主内部头文件。Qt6.4 的 `qt_add_plugin` 后使用 `target_sources` 显式加入源文件。初始化任务取消、停止等待与动态库释放顺序同样适用。两端 API 数字相同，但插件不能混载。

客户端 `HostServices::logs().write(level,message)` 显式双写主程序和插件数据目录的日志，支持后台线程；结果分别包含两处错误。共用公开头文件位于 `src/shared/include/`，安装后的 SDK 已包含它们。语音项目另需根 `src/shared/voice-chat/` 的业务协议源码。

PowerShell示例，Qt路径替换为实际动态 SDK：

```powershell
$qtSdk = 'C:/Qt/6.8.3/msvc2022_64'
$apiVersion = ((Get-Content VERSION -Raw).Trim().Split('.')[0..1] -join '.')
$apiSdk = Join-Path (Get-Location) "dist/ZTGUI-api-v$apiVersion"
cmake -S src/client/api -B cmake-build-api -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$qtSdk"
cmake --install cmake-build-api --prefix "$apiSdk"
cmake -S src/client/plugins/myplugin -B cmake-build-plugin-myplugin -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$apiSdk;$qtSdk"
cmake --build cmake-build-plugin-myplugin --config Release --parallel
```

每步检查退出码，失败先修复，不使用旧产物。独立构建只需安装后的SDK及匹配工具链；不需要主程序源码。用户指定路径时同步修改`-S`参数。

将产物DLL复制到`ZTGUI.exe`旁的`plugins/`，并部署插件使用的Qt模块及第三方运行依赖。macOS使用`ZTGUI.app/Contents/MacOS/plugins/`。启动时按文件名扫描该目录直接子文件，检查IID和精确API版本，然后调用`definition()`注册菜单；增减插件后重启，宿主不链接具体插件。缺少目录正常启动，错误写入宿主日志；无热加载或在线卸载管理。

验证独立构建、真实动态加载、菜单/页面创建、父控件所有权和资源清理。涉及协议或音频时增加相关测试；不要因仅修改说明重复运行完整GUI回归。测试辅助程序不进入发行包。

交付时说明源码目录、API版本、构建命令、实际产物位置、运行依赖和真实加载验证结果。插件文档放在各项目的 `docs/` 并由插件自己维护。语音项目的部署与协议见[语音插件文档](../../../src/client/plugins/voice-chat/docs/voice-chat.md)。
