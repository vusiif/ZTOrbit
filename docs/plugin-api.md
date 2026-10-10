# 左侧栏插件 API v0.2

本 API 为插件贡献左侧菜单和对应的 Qt Widgets 页面提供统一入口。宿主按菜单 ID 导航，插件只声明菜单与页面工厂，不直接操作左侧列表或页面堆栈。

当前实现覆盖注册、校验、菜单排序、懒创建页面、页面缓存和注销清理。默认未注册扩展，界面仍只有 ZeroTier；设置中的“插件”入口仍待后续管理界面接入。

## 独立 SDK

`src/api/` 只包含公开接口和 SDK 构建配置，不依赖 `src/main/`。`src/main/` 包含主程序、ZeroTier 业务与插件注册表，依赖 CMake 目标 `ZTGUI::Api`。API 目前是头文件 SDK，无需生成或链接额外的 API DLL。

在仓库根目录独立配置并安装 SDK（不构建主程序）：

```powershell
cmake -S src/api -B cmake-build-api -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --install cmake-build-api --prefix dist/ZTGUI-api-v0.2
```

安装目录包含 `include/ztgui/PluginApi.h`、生成的 `include/ztgui/ApiVersion.h` 和 `lib/cmake/ZTGUIPluginApi/`。插件开发者只需这份安装后的 SDK 及匹配的 Qt/编译工具链，无需主程序源码。源目录单独配置时仍从仓库根目录的 `VERSION` 读取版本。

独立插件项目的 CMake 示例：

```cmake
cmake_minimum_required(VERSION 3.21.1)
project(MyPlugin LANGUAGES CXX)
find_package(ZTGUIPluginApi 0.2 EXACT CONFIG REQUIRED)
add_library(my_plugin MODULE Plugin.cpp)
target_link_libraries(my_plugin PRIVATE ZTGUI::Api)
```

配置插件时，将安装后的 SDK 和 Qt SDK 路径加入 `CMAKE_PREFIX_PATH`。插件源码使用 `#include <ztgui/PluginApi.h>`；该目标会提供头文件路径、C++17 要求及 Qt Widgets 依赖。这个构建示例仅说明依赖方式；外部 DLL 的入口协议及宿主加载器尚未实现。

主程序当前为 `0.2.2`，API 为 `0.2`。正式发布前首段固定 0，第二段表示核心/API 代际，第三段仅记录主程序非核心更新、修复和优化。API 版本常量 `ApiMajor`、`ApiMinor`、`ApiVersion` 和 CMake 包版本都从 `VERSION` 前两段生成。插件必须精确匹配 API 两段；`0.2.x` 主程序更新不改变 API 兼容性。Qt、编译器、架构和运行库的二进制兼容性仍需单独满足。

## 层次与边界

```text
插件定义 / 后续插件加载器
           │ registerPlugin(PluginDefinition)
           ▼
     PluginRegistry
           │ menusChanged / menus() / page(menuId, parent)
           ▼
       MainWindow
           │ 稳定 ID → 左侧菜单与页面
           ▼
       QWidget 页面
```

| 文件 | 职责 |
| --- | --- |
| `src/api/include/ztgui/PluginApi.h` | 公开数据结构、API 版本、错误类型与页面工厂签名 |
| `src/main/plugins/PluginRegistry.h/.cpp` | 校验与保存定义、菜单查询、页面创建与资源释放 |
| `src/main/MainWindow.h/.cpp` | 侧栏展示、按 ID 切换、动画与失败回退 |

`PluginDefinition` 是宿主进程内的 C++ API，当前适用于随程序编译的扩展。它包含 Qt 类型和 `std::function`，不能作为跨进程协议或稳定的 DLL ABI。API 版本用于能力兼容校验，不代表二进制兼容。

项目已有静态 Qt 单 EXE 构建路径。外部插件的发现、安装、启用状态、动态库加载、依赖检查和卸载策略属于后续加载层；本次没有加入 DLL 扫描或热卸载。后续加载器应将插件转换成相同的 `PluginDefinition`，并维持代码与资源的生命周期。若需要独立进程或脚本插件，应增加序列化协议和宿主页面适配器，不能直接传递 QWidget 或页面工厂。

## 插件和菜单定义

| 类型 | 字段 | 约定 |
| --- | --- | --- |
| `PluginInfo` | `id` | 全局唯一；建议使用反向域名，例如 `com.example.monitor` |
| | `name`, `version` | 非空显示名称、插件自身的版本字符串 |
| | `apiMajor`, `apiMinor` | 默认 0.2；预发布阶段两段必须与宿主完全一致，主程序后缀版本不参与 API 校验 |
| `MenuContribution` | `id` | 插件内唯一，例如 `overview` |
| | `title` | 非空菜单标题；允许与其他菜单重名 |
| | `icon`, `toolTip` | 可选图标、提示文字 |
| | `order` | 默认 100；数值越小越靠前 |
| | `createPage` | 必填；`QWidget *(QWidget *parent)` 页面工厂 |
| `PluginDefinition` | `info`, `menus` | 插件信息和至少一个菜单 |

插件 ID 和本地菜单 ID 都匹配 `[a-z0-9][a-z0-9._-]{0,127}`，不允许 `/`、空白或中文。全局菜单 ID 由宿主拼接为 `pluginId/menuId`，例如 `com.example.monitor/overview`。展示标题可以使用中文。

内置 ZeroTier 固定为 `builtin/zerotier`，始终放在第一项。`builtin` 是保留的插件 ID，不能注册；内置页面也不属于插件注册表，不能通过注销插件删除。插件菜单按 `order` 排序，同序号时按全局菜单 ID 排序，与注册顺序无关。

## 宿主接口

通过 `MainWindow::pluginRegistry()` 获取该窗口的注册表。所有接口和信号都在 GUI 线程使用；注册表也应在 GUI 线程创建。

| 接口 | 行为 |
| --- | --- |
| `registerPlugin(PluginDefinition)` → `Result` | 按值保存定义和工厂捕获；完整校验后一次性注册；失败不添加任何菜单 |
| `unregisterPlugin(pluginId)` → `Result` | 同步销毁已创建的页面，再释放工厂和定义，最后通知菜单变化 |
| `plugins()` → `QList<PluginInfo>` | 按插件 ID 排序的信息快照 |
| `menus()` → `QList<RegisteredMenu>` | 排序后的菜单快照；包含全局 ID、插件 ID、本地 ID 和显示字段 |
| `page(menuId, parent)` → `PageResult` | 首次调用创建，之后返回缓存页面；缓存被销毁后允许重新创建 |
| `menusChanged()` | 注册、注销成功后的信号；宿主重新查询菜单，并保持当前菜单 ID |
| `pageCreationFailed(menuId, message)` | 工厂返回无效控件或抛出异常时的信号；主窗口写日志 |

注册重复插件 ID 会失败。需要替换插件时，先注销，再注册新的定义；当前没有在线修改菜单的接口。

`Result::ok()` 判断成功；`Result::error` 是可供程序判断的错误码，`message` 是可展示或记录的原因。`PageResult` 包含 `page` 和 `result`，失败时 `page == nullptr`。

| 错误码 | 含义 |
| --- | --- |
| `None` | 成功 |
| `WrongThread` | 注册、注销或创建页面不在 GUI 线程 |
| `Busy` | 页面工厂或清理期间重入注册、注销、页面创建 |
| `InvalidPlugin` | 插件 ID、名称或版本无效，或使用保留 ID |
| `IncompatibleApi` | API 主版本或核心版本与宿主不一致 |
| `DuplicatePlugin` | 插件 ID 已注册 |
| `InvalidMenu` | 菜单为空、ID 无效／重复、标题为空或工厂缺失 |
| `NotFound` | 插件或菜单不存在 |
| `PageCreationFailed` | 页面创建失败，或没有有效的父控件 |

## 页面与生命周期

1. 注册时不创建页面；首次点击菜单才执行页面工厂。
2. 工厂必须返回 `new` 创建的、直接由传入 `parent` 持有的控件。不能返回父控件、借用宿主已有控件或与其他菜单共用页面。页面内部的布局和滚动由插件负责。
3. 同一菜单在一个注册表中缓存一个页面，供同一宿主使用。切换菜单不会销毁页面，输入和滚动位置由控件保留。
4. 工厂应快速完成 UI 构造，耗时操作使用异步方式。工厂或页面析构期间不能重入修改注册表；这类调用返回 `Busy`。
5. 工厂返回空指针、无效控件或抛出异常时，宿主保留之前的页面和选中菜单。下次点击允许重试。返回已有控件时不销毁该控件；工厂抛出异常前创建的临时资源由工厂负责清理。
6. 注销插件时，页面同步销毁，页面工厂的捕获随后释放。插件代码、共享资源或未来加载器句柄必须至少活到这两步完成，不能只依赖注册调用时的栈变量。
7. 注销当前显示的插件后，导航回到 ZeroTier；注销其他插件或插入更靠前的菜单时，当前页面按 ID 保持。
8. 窗口关闭时先销毁插件页面和工厂，再销毁主窗口 UI。异步回调应使用 QObject 上下文或 QPointer，确保页面销毁后不继续访问控件。

插件运行在宿主进程中，这个 API 不提供权限隔离。页面工厂只接收父控件，宿主不通过该接口提供 ZeroTier 命令执行、管理员脚本或其他业务服务。后续业务能力应以明确的服务接口另行设计。

## 最小接入示例

下面代码由宿主启动代码或后续加载器调用，定义两个菜单。插件页面实现只需要公开 API；示例中的 `MainWindow` 用于演示宿主注册入口。

```cpp
#include "MainWindow.h"
#include "plugins/PluginRegistry.h"
#include <QLabel>
#include <utility>

ZtGui::Plugins::Result registerMonitor(MainWindow &window)
{
    using namespace ZtGui::Plugins;
    PluginDefinition plugin;
    plugin.info = {"com.example.monitor", QStringLiteral("网络监控"), "1.0.0"};

    MenuContribution overview;
    overview.id = "overview";
    overview.title = QStringLiteral("网络监控");
    overview.toolTip = QStringLiteral("查看连接监控");
    overview.order = 100;
    overview.createPage = [](QWidget *parent) -> QWidget * {
        return new QLabel(QStringLiteral("网络监控概览"), parent);
    };

    MenuContribution history;
    history.id = "history";
    history.title = QStringLiteral("连接记录");
    history.order = 110;
    history.createPage = [](QWidget *parent) -> QWidget * {
        return new QLabel(QStringLiteral("连接历史"), parent);
    };

    plugin.menus = {overview, history};
    return window.pluginRegistry()->registerPlugin(std::move(plugin));
}
```

移除该插件：

```cpp
const auto result = window.pluginRegistry()->unregisterPlugin("com.example.monitor");
if (!result.ok()) window.appendOutput(result.message + '\n');
```

## 验证

回归测试覆盖原子注册、API 版本拒绝、命名空间与排序、懒创建、状态缓存、菜单位置变化、当前／非当前插件移除、工厂失败重试、重入拒绝、页面所有权、后台线程拒绝和页面先于工厂资源销毁。测试插件只在测试中注册，不进入正式应用的默认导航。
