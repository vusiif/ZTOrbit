---
name: ztgui-plugin-dev
description: 在本项目中创建、编写、修改和独立编译 ZTGUI 的 C++/Qt 动态插件，通过公开 SDK 提供菜单与页面。
---

# ZTGUI 插件开发

## 接口、目录与版本

本文件所在目录向上三级是仓库根目录。先读取根 `AGENTS.md`、`VERSION`（客户端）、`SERVER_VERSION`（服务端）、[接口文档](../../../docs/plugin-api.md) 和 `src/client/api/include/ztgui/` 的公开接口。插件默认位于 `src/plugins/<name>/client/`；用户指定其他目录时使用指定路径，例如语音插件的 `src/plugins/voice-chat/client/`。

插件只能依赖公开 SDK 和必要第三方库，不包含 `src/client/main/` 内部头文件。测试放在 `src/tests/`。需要宿主业务能力时明确设计公开服务接口，不访问宿主内部对象绕过 API。

程序版本号由开发者管理，未经开发者明确指示不得自动修改 `VERSION` 或 `SERVER_VERSION`。客户端与服务端版本独立：客户端使用 `VERSION`（当前 `0.5.1-alpha`），服务端使用客户端侧指定的 `SERVER_VERSION`（当前 `0.2.1-beta`）；同端 API 取本端程序版本前两段数字并保留相同预发布后缀（客户端 `0.5-alpha`，服务端 `0.2-beta`）。未指定新版本的变更记录在 `CHANGELOG.md` 的未发布部分。插件自身版本独立于宿主；保留 `PluginInfo` 的默认 API 字段，不重复定义版本常量或生成头文件。

插件自身版本号变化时，必须按[编译与打包技能](../ztgui-build/SKILL.md)重新编译对应插件并生成新 `.ztplugin`，核对二进制与包声明版本一致；仅改版本号也不能复用旧二进制，不需要再等待用户单独要求编译。只构建该客户端／服务端插件，不联动其他未变化的插件。构建结束及时删除 `.previous-*` 等旧产物备份，不保留构建备份。

统一使用动态 Qt，已停止静态 Qt/单 EXE 构建。宿主与插件必须使用匹配的 Qt、编译器、架构、运行库和构建模式。`PluginDefinition` 是进程内 C++ 接口，不承诺跨工具链二进制兼容。

## 独立项目示例

目录包含 `CMakeLists.txt`、`Plugin.cpp` 和 `plugin.json.in`。这里以当前客户端 API `0.5-alpha` 为例，实际要求应根据根 `VERSION` 或 `SERVER_VERSION` 填写。

```cmake
cmake_minimum_required(VERSION 3.21.1)
project(myplugin LANGUAGES CXX)
set(CMAKE_AUTOMOC ON)
find_package(ZTGUIPluginApi 0.5 EXACT CONFIG REQUIRED)
configure_file(plugin.json.in "${CMAKE_CURRENT_BINARY_DIR}/plugin.json" @ONLY)
qt_add_plugin(myplugin SHARED CLASS_NAME vusiif::ztgui::example::MyPlugin Plugin.cpp)
target_include_directories(myplugin PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(myplugin PRIVATE ZTGUI::Api)
qt_add_resources(myplugin plugin_page PREFIX /myplugin FILES Page.qml)
ztgui_package_plugin(myplugin HOST client ID myplugin NAME "我的插件" VERSION "0.1.0"
    API "${ZTGUI_API_VERSION}" IID "vusiif.ztgui.api.PluginInterface/3.0")
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
#include <ztgui/PluginPage.h>
namespace vusiif::ztgui::example {
class MyPlugin final : public QObject, public vusiif::ztgui::api::PluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ZTGUI_PLUGIN_IID FILE "plugin.json")
    Q_INTERFACES(vusiif::ztgui::api::PluginInterface)
public:
    vusiif::ztgui::api::PluginDefinition definition() override {
        using namespace vusiif::ztgui::api;
        PluginDefinition plugin;
        plugin.info = {"myplugin", QStringLiteral("我的插件"), "0.1.0"};
        MenuContribution menu;
        menu.id = "overview";
        menu.title = QStringLiteral("我的插件");
        menu.createPage = [](QQmlEngine *engine, QQuickItem *parent) {
            return createQmlPage(engine, parent, QUrl("qrc:/myplugin/Page.qml"));
        };
        plugin.menus.append(menu);
        return plugin;
    }
};
}
#include "Plugin.moc"
```

`Page.qml` 示例：

```qml
import QtQuick
import QtQuick.Controls
Pane {
    Label { anchors.centerIn: parent; text: "插件页面" }
}
```

需要C++控制器时使用页面的required属性传入QObject；控制器由页面持有。不要注册DLL所有的全局QML类型或单例，避免卸载后引擎仍引用插件代码。SDK的 `createQmlPage()` 支持初始属性并在完成组件前设置父项；工厂失败返回空指针。QML引擎为借用对象，插件不能销毁它或改变宿主的导入路径。

`ZTGUI::Api` 提供公开头文件、C++17 和 Qt Quick/QML 依赖。插件公共头文件若暴露 SDK 类型，链接作用域采用 `PUBLIC`；只在内部使用时采用 `PRIVATE`。

插件 ID 匹配 `[A-Za-z0-9_-]{1,128}`，冲突不区分大小写；菜单 ID 匹配 `[a-z0-9][a-z0-9._-]{0,127}`，插件ID不能为`builtin`。页面工厂接收借用的 `QQmlEngine*` 与 `QQuickItem* parent`，在GUI线程返回新建、QObject parent 与 parentItem 均为传入parent的可视项，不能借用宿主已有可视项或跨菜单共用页面。耗时工作放在工作线程，回调绑定QObject上下文。存在长期业务时实现独立 `PluginStopInterface/1.0`，在 GUI 线程快速发起停止，并在会话、线程、回调和服务租约排空后完成回调；失败时宿主保留 DLL 与提供者。页面析构与 `onDestroy()` 只做已停止状态下的有界清理。旧普通插件仍须在析构前确保业务结束。

公开插件类型统一在 `vusiif::ztgui::api`，工具接口与取消类型统一在 `vusiif::ztgui::util`。插件项目自己的类型使用 `vusiif::ztgui::<模块名>`，例如语音项目的 `voice_chat`；宿主内部实现使用 `plugins/ui/zerotier/platform` 等模块。不要创建旧命名空间别名或兼容入口。

需要初始化、清理、菜单切换通知或宿主工具时，实现 `<ztgui/PluginLifecycle.h>` 的 `vusiif::ztgui::api::PluginLifecycleInterface` 并添加到 `Q_INTERFACES`。唯一初始化签名为 `api::CreateTask onCreate(util::HostServices&, const util::CancellationToken&)`。在GUI线程快速返回任务，空任务表示立即就绪；令牌按值捕获，后台任务在步骤之间检查 `isCancellationRequested()`，取消后清理部分资源并返回。任务不得访问GUI控件或承担永久业务循环，应捕获独立共享状态并保证能够结束。宿主关闭时请求取消、跳过未开始的初始化，并异步等待全部任务结束，期间GUI响应并显示待结束的插件；不能取消的第三方调用仍须等返回。

`onExitedTo(api::MenuLocation)` 先于目标插件的 `onEnteredFrom(api::MenuLocation)`，均在GUI线程执行，不得重入导航或修改注册表。`onDestroy()` 在任务完成、页面/工厂释放和查询取消后、根对象与DLL卸载前调用，应允许初始化失败或取消时资源不完整。支持异步停止的插件先完成 `requestStop(context, completed)`，再销毁页面和工厂；停止失败不得强行卸载。旧接口和单参数onCreate已移除，QML页面IID为 `vusiif.ztgui.api.PluginInterface/3.0` 与 `vusiif.ztgui.api.PluginLifecycleInterface/3.0`，旧插件必须重编；程序版本由开发者另行管理。

保存 `onCreate()` 注入的 `HostServices` 借用指针可获得文件和ZeroTier服务，有效至 `onDestroy()` 返回。`files().readFile/writeFile` 支持后台线程，使用程序目录下的 `plugins-data/<PluginInfo.name>/`，宿主自动拼接插件名称，调用只传目录内相对路径；写入原子替换，禁止绝对路径、`..` 或链接路径。名称须合法、稳定且唯一，中文名称可用，同名插件会共用目录，改名需自行迁移数据。工具整文件读写，适用于配置和小型缓存，大文件传输应使用流式IO。`zeroTier().getZTInfo/getZTPeers/getZTNetworks/getZTMoons` 在GUI线程接受QObject上下文和异步 `JsonCallback`，Info返回JSON对象，其余返回数组，失败使用 `JsonResult::error`。上下文销毁或插件卸载时取消请求，不能在 `onDestroy()` 发起查询。详细签名和生命周期顺序见公开API文档。

## 安装 SDK、构建与启用

服务端插件使用 `src/plugins/<name>/server/` 的独立 Maven 项目，依赖 `vusiif.ztgui:ztserver-api`（`provided`，JDK 25），输出 `plugins/*.jar` 并内嵌 `META-INF/ztserver-plugin.json`。先读 [服务端 API](../../../docs/server-api.md)，通过 Java `ServerPlugin` 生命周期、宿主事件线程和统一路由接入；不能引用宿主内部实现类。旧 C++ `.ztplugin` 只存在于 `src/server-old/`，不再加载。两端 API 数字相同但不通用，客户端 C++ 插件与 Java 服务端插件不能混载。

客户端 `HostServices::logs().write(level,message)` 显式双写主程序和插件数据目录的日志，支持后台线程；结果分别包含两处错误。共用公开头文件位于 `src/shared/include/`，安装后的 SDK 已包含它们。插件项目的共享 C++ 代码放在 `src/plugins/<name>/shared/`，可被客户端与归档 C++ 服务端引用；Java 服务端插件在 `server/` 内按线协议重新实现。语音项目使用 `src/plugins/voice-chat/shared/` 的协议源码。

PowerShell示例，Qt路径替换为实际动态 SDK：

```powershell
$qtSdk = 'C:/Qt/6.8.3/msvc2022_64'
$version = (Get-Content VERSION -Raw).Trim()
$match = [regex]::Match($version, '^0\.([0-9]+)\.[0-9]+(-[0-9A-Za-z.-]+)?$')
if (!$match.Success) { throw 'VERSION must be 0.<core>.<patch>[-qualifier]' }
$apiVersion = "0.$($match.Groups[1].Value)$($match.Groups[2].Value)"
$apiSdk = Join-Path (Get-Location) "build/dist/ZTGUI-api-v$apiVersion"
cmake -S src/client/api -B build/cmake-build-api -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$qtSdk"
cmake --build build/cmake-build-api --config Release --target ztplugin_pack
cmake --install build/cmake-build-api --config Release --prefix "$apiSdk"
cmake -S src/plugins/myplugin/client -B build/cmake-build-plugin-myplugin -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$apiSdk;$qtSdk"
cmake --build build/cmake-build-plugin-myplugin --config Release --parallel
```

每步检查退出码，失败先修复，不使用旧产物。独立构建只需安装后的SDK及匹配工具链；不需要主程序源码。用户指定路径时同步修改`-S`参数。

将编译后生成的 `.ztplugin` 放入客户端宿主 `plugins/`；正式客户端宿主不加载裸 DLL／SO／DYLIB。Java 服务端插件把 `.jar` 放入服务端运行目录 `plugins/`，宿主只在启动时扫描，增减后重启。客户端插件菜单支持运行时导入／加载／卸载；卸载保留文件和数据，并持久禁用，手动加载成功恢复启动自动加载。加密包保留可读基本信息，客户端 SDK 提供封装工具与 CMake 辅助函数，详见 [API 文档](../../../docs/plugin-api.md) 和 [多中转通信](../../../docs/network-relays.md)。

插件只能通过已安装的公开 API SDK 和宿主管理的服务进行显式依赖；必须在认证的 ZTPK3 清单中声明提供者、语义版本范围和服务接口版本。禁止包含其他插件的私有头、链接其实现 DLL 或私自查找对象。ZeroTier 操作只读。`broadcastDataTo(networkId, memberIds, data, ...)` 固定走网络 `.1`，`broadcastDataBalancedTo` 使用多中转稳定分配，失败直发；`getNetworkRelays` 查询快照，`subscribeNetworkData` 接收本机同 ID 插件数据。回调限事件线程；卸载会取消查询、订阅及发送任务，正在执行的回调返回前不释放动态库。宿主和插件仍需匹配动态 Qt／工具链／架构／构建模式。

验证独立构建、真实动态加载、菜单/页面创建、父控件所有权和资源清理。涉及协议或音频时增加相关测试；不要因仅修改说明重复运行完整GUI回归。测试辅助程序不进入发行包。

交付时说明源码目录、API版本、构建命令、实际产物位置、运行依赖和真实加载验证结果。插件文档放在各项目的 `client/docs/` 或 `server/docs/` 并由插件自己维护。语音项目的部署与协议见[语音插件文档](../../../src/plugins/voice-chat/client/docs/voice-chat.md)。

## v3 服务与独立运行环境

普通无依赖插件的 helper 默认仍生成 v2。使用 `SERVICE_MANIFEST /absolute/path/manifest.json` 生成 ZTPK3；清单包含 `execution`、`dependencies` 和 `provides`，进程内插件的 Qt JSON 元数据必须包含完全一致的依赖和服务声明。worker-process 使用 `ztgui-service/1`，无需 Qt 插件入口 IID；允许没有菜单的逻辑插件。旧宿主必须通过新 magic 拒绝该包。

Windows worker 入口必须与运行库放在 `runtime/bin/`，并使用包内 qt.conf 固定 Qt 插件位置。不能用 PATH 替代应用目录部署，否则启动时可能先找到系统旧 CRT。旧 service/ 入口布局需要重新封装；普通进程内插件仍禁止业务文件占用 runtime/ 保留目录。

慢硬件准备使用 Worker::deferReady()，通过 onConnected 在 IPC 认证后启动后台任务；完成后回到事件线程调用 markReady()。宿主准备阶段限时60秒，完成前不会发布服务或执行消费者，不能在建立 IPC 前同步枚举设备。保持事件线程响应，并为不能中断的驱动调用保留进程级有界停止。

通过 `dynamic_cast<util::ServiceAccess *>(&host)` 查询独立服务扩展，避免改变 HostServices 原虚表。只能 acquire 清单声明且就绪的提供者服务；调用、订阅和 close 均在事件线程执行并绑定 QObject 上下文。close 完成表示远端会话及在途请求已排空；不要在卸载后保留回调或裸提供者对象。接口与限制见[服务契约](../../../docs/plugin-services.md)。

设备枚举、Qt Multimedia、FFmpeg、Opus 等媒体实现放在独立工作进程。语音只链接安装后的 `ZTGUI::MediaApi`；媒体项目在 `src/plugins/media/client/`，API 在其 `api/`，引擎在 `service/`。服务进程携带自己的 Qt 基础库、后端和编译器运行库，运行环境只作用于子进程；宿主不得调用 activateRuntime、添加其 Qt 路径或预加载其业务库。

进程内插件继续匹配宿主 Qt／编译器／架构／构建模式，不能携带额外 Qt 模块和后端；旧媒体包必须迁移重编。旧普通插件的非 Qt 私有库仍可能保留到进程退出，此兼容路径不提供媒体完全卸载保证。归档的 C++ 服务端仅保留 v1/v2 包；Java 服务端插件是 `plugins/*.jar`（manifest `apiVersion`），不使用 Qt IID。Windows x64 是当前工作进程验证平台。包、许可证和安装 SDK 构建见[运行依赖交付](../../../docs/plugin-runtime-separation.md)。


## 通用媒体引擎与扩展 SDK

媒体包 2.0.0 提供 media.session / 1；Media API SDK 2.0.0 与 Engine SDK 1.0.0 分开交付。业务界面插件显式链接 ZTGUI::Api 与 ZTGUI::MediaApi；无界面消费者只需 Core/Network 和宿主 Services 组件。完整 FFmpeg 仅开放给 worker 侧 Engine SDK，扩展随媒体包认证清单交付。构建媒体使用 build-media-windows.ps1，可传 FfmpegRoot 复用受控依赖 SDK。API 目录及 ZIP 为 build/dist/ZTGUI-media-api-v2，Engine SDK 为 build/dist/ZTGUI-media-engine-sdk-v1。测试使用 run-media-tests.ps1，必须指定实际 QtRoot，避免 Qt 平台插件路径错误导致阻塞弹窗。详细契约由媒体项目 client/docs/media.md 维护。
