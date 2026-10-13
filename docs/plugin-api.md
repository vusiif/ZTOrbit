# 插件 API（开发代际 0.3）

公开 SDK 位于 `src/client/api/`，不依赖 `src/client/main/`。插件通过菜单定义和 Qt Quick/QML 页面工厂接入宿主，可选生命周期负责初始化、导航通知和资源释放；文件与 ZeroTier 工具由宿主注入。

## 命名空间与目录

| 命名空间 | 职责 | 位置 |
| --- | --- | --- |
| `vusiif::ztgui::api` | 插件信息、菜单、结果、入口及生命周期接口 | `src/client/api/include/ztgui/` |
| `vusiif::ztgui::util` | 文件、ZeroTier 查询、宿主服务和取消工具 | 公开定义在 `src/client/api/`，实现位于 `src/client/main/` |
| `vusiif::ztgui::plugins` | 动态加载器、注册表、准备状态及清理 | `src/client/main/plugins/` |
| `vusiif::ztgui::ui` | QML控制器、增量数据模型与主题组件 | `src/client/main/` |
| `vusiif::ztgui::zerotier` | ZeroTier 客户端访问、轮询及配置包 | `src/client/main/` |
| `vusiif::ztgui::platform` | 平台脚本与授权辅助 | `src/client/main/` |
| `vusiif::ztgui::voice_chat` | 语音客户端、服务端及共享协议 | `src/plugins/voice-chat/` |

头文件路径仍使用 `<ztgui/...>`，CMake 目标仍为 `ZTGUI::Api`。不提供旧命名空间别名。宿主的 `PluginRegistry`、`PluginLoader` 和 `HostServicesImpl` 不属于公开 SDK，插件不能包含其实现头文件。

## QML页面与二进制边界

页面可通过 `<ztgui/PluginPage.h>` 的 `createQmlPage(engine,parent,source,initialProperties)` 从插件自己的资源创建。C++控制器通过required属性注入，由页面持有；页面先于工厂和DLL销毁，宿主清理引擎组件缓存后才卸载库。插件不能注册DLL所有的全局QML类型或单例，也不能依赖宿主内部QML模块。可使用Qt Quick Controls并继承应用窗口palette。

本次迁移保持根 `VERSION` 与API数字，由新入口／生命周期IID `/2.0` 区分页面ABI。旧 `/1.0` Widgets DLL在实例化前拒绝，必须迁移页面工厂并重编。SDK依赖Qt Quick/Qml，已移除Widgets依赖；应用保留Widgets仅供Windows原生托盘菜单后备实现。

## 当前入口与生命周期

`PluginInterface.h` 的入口位于 `vusiif::ztgui::api`：

```cpp
class PluginInterface {
public:
    virtual ~PluginInterface() = default;
    virtual PluginDefinition definition() = 0;
};
```

`PluginLifecycle.h` 的可选生命周期位于同一命名空间，全部有默认实现，插件只覆盖需要的方法：

```cpp
class PluginLifecycleInterface {
public:
    virtual ~PluginLifecycleInterface() = default;
    virtual CreateTask onCreate(util::HostServices &services,
                                const util::CancellationToken &cancellation);
    virtual void onDestroy();
    virtual void onEnteredFrom(const MenuLocation &previous);
    virtual void onExitedTo(const MenuLocation &next);
};

struct CreateResult { bool success = true; QString message; };
using CreateTask = std::function<CreateResult()>;
```

初始化只有这一个 `onCreate` 签名。旧的初始化/预处理接口、单参数重载、结果转换与入口优先级分支已移除。

所有钩子在 GUI 线程调用；`onCreate()` 在成功注册后调用一次，快速返回有限时间的后台准备任务。默认空任务表示立即就绪。实际任务由独立线程池执行，最多同时运行2项；任务不能访问GUI对象或作为永久录音、视频、收包循环。`CreateResult{false, message}` 或异常表示失败，菜单保持禁用并显示原因。

`MenuLocation` 包含 `PluginInfo plugin`、`RegisteredMenu menu` 和 `bool builtin`，可区分同一插件的多个菜单。内置 ZeroTier 使用 `builtin/zerotier`，`builtin == true`。

每次成功改变菜单ID，按 `A.onExitedTo(B) → 页面切换 → B.onEnteredFrom(A)` 在GUI线程执行。同一插件不同菜单也触发；重复选择当前菜单、列表重建和页面创建失败不触发。菜单切换保留页面与任务，是否暂停预览、语音或传输由插件决定。钩子应快速返回，重入导航或注册表修改被拒绝，异常记录日志后继续切换。

## 取消与关闭

`vusiif::ztgui::util::CancellationToken` 可复制且线程安全。入口参数的令牌必须按值捕获到返回任务，不能保存参数引用。宿主关闭或内部卸载时请求取消，任务在步骤之间检查 `isCancellationRequested()`，清理部分资源后返回。`CancellationSource` 提供 `token()` 和幂等的 `requestCancellation()`，由宿主持有。

```cpp
api::CreateTask onCreate(util::HostServices &services,
                         const util::CancellationToken &cancellation) override {
    host = &services;
    return [state = sharedState, cancellation] {
        if (cancellation.isCancellationRequested()) return api::CreateResult{false, "cancelled"};
        state->prepareFirstStep();
        if (cancellation.isCancellationRequested()) return api::CreateResult{false, "cancelled"};
        state->prepareSecondStep();
        return api::CreateResult{};
    };
}
```

正常关闭窗口时，QML窗口拒绝第一次关闭事件，禁用新操作，显示退出状态及待结束的插件名称；请求初始化取消，跳过未开始的任务。完成通知和20ms非阻塞线程池检查推进退出，GUI事件循环保持运行。重复关闭只发起一次停机；等待正在执行的系统操作及插件任务真正结束后才退出。

清理顺序为：请求取消 → 确认任务结束 → 退出当前插件菜单 → 销毁QML页面、控制器和工厂 → 清理引擎组件缓存 → 取消服务查询 → `onDestroy()` → 销毁插件根对象并卸载库。初始化失败、取消或未执行时也会清理生命周期，`onDestroy()` 应允许资源尚未准备或仅部分准备。

取消不能强行打断不可取消的第三方调用，耗时IO应有自己的超时或取消方式。页面析构和 `onDestroy()` 仍须及时同步停止自身业务线程，不能在DLL卸载后留下线程、回调或函数对象。直接析构加载器或绕过窗口的quit路径保留阻塞等待兜底。长期业务的异步停机尚待扩展。

宿主内部 `PluginLoader::requestShutdown()` 非阻塞，`shutdownProgress(QStringList)` 报告待结束的插件名，`shutdownFinished()` 表示初始化和线程池已结束。调用者须保持加载器和注册表存活至完成，再执行析构清理。内部单插件 `unload(pluginId, registry)` 仍同步等待，整体停机期间拒绝扫描和卸载。当前没有目录监听或热加载管理界面。

## 数据结构与约束

| 类型 | 字段/签名 | 约定 |
| --- | --- | --- |
| `PluginInfo` | `id`, `name`, `version`, `apiMajor`, `apiMinor` | ID全局唯一；名称与版本非空，API字段默认从SDK生成常量取得 |
| `MenuContribution` | `id`, `title`, `icon`, `toolTip`, `order`, `createPage` | 本地ID唯一，标题非空，工厂必填；默认order为100 |
| `PluginDefinition` | `PluginInfo info`, `QList<MenuContribution> menus` | 至少一个菜单 |
| `RegisteredMenu` | `id`, `pluginId`, `localId`及显示字段 | 全局ID为 `pluginId/menuId` |
| `Result` | `Error error`, `QString message`, `ok()` | 错误码可供程序判断，message供展示或日志 |
| `PageResult` | `QQuickItem *page`, `Result result` | 失败时page为空 |
| `PageFactory` | `std::function<QQuickItem *(QQmlEngine *engine, QQuickItem *parent)>` | 在GUI线程返回新建、QObject parent与parentItem均为parent的可视项 |

插件ID与本地菜单ID匹配 `[a-z0-9][a-z0-9._-]{0,127}`。`builtin` 为保留插件ID，内置ZeroTier始终位于首项；插件菜单按order再按全局ID排序。

注册不创建页面，首次访问才调用工厂，一个菜单缓存一个页面。切换菜单保留输入及滚动状态；缓存页面销毁后可重建。工厂不能返回父项、借用已有可视项或与其他菜单共用页面，不能跨宿主复用。工厂失败保留原页面和选中菜单，下次访问可重试。页面析构先同步停止自己的线程，随后才释放工厂捕获和插件库。

错误码：`None`、`WrongThread`、`Busy`、`InvalidPlugin`、`IncompatibleApi`、`DuplicatePlugin`、`InvalidMenu`、`NotFound`、`PageCreationFailed`。注册、注销、创建页面和导航期间的线程/重入检查由宿主注册表执行。

## 宿主工具

工具定义都位于 `vusiif::ztgui::util`，由 `onCreate` 注入 `HostServices`。服务为借用引用，可保存指针并交给页面使用，有效至 `onDestroy()` 返回，不能delete。公开SDK只有抽象定义，宿主提供实现。

### 文件

```cpp
FileResult readFile(const QString &relativePath);
FileResult writeFile(const QString &relativePath, const QByteArray &data);
```

通过 `host->files()` 使用。`FileResult` 包含 `QByteArray data`、`QString error` 和 `ok()`；空文件读取成功，写入成功data为空。字节不做编码转换。

根目录为 `applicationDirPath()/plugins-data/<PluginInfo.name>/`，由宿主绑定。例如程序为 `C:/ztgui/ZTGUI.exe`、插件名为 `chat`，读取 `file1.txt` 就是 `C:/ztgui/plugins-data/chat/file1.txt`；插件不再传名称前缀。现有语音插件使用 `plugins-data/语音聊天/`。

名称支持中文和内部空格，但必须是普通目录名；非法路径字符、设备名、首尾空白、末尾句点、`.`/`..` 或空名会使文件操作返回错误。名称应稳定且唯一，同名插件共用目录，大小写差异不能保证跨平台分离；改名后使用新目录，迁移需明确数据归属。

写入自动创建目录并通过 `QSaveFile` 原子替换；文件操作可在后台线程调用，宿主串行化操作。拒绝绝对路径、`..`、盘符、链接/目录联接以及非普通文件，不能通过工具跨插件目录访问。权限不足或文件不存在通过error返回。整文件读写适合配置和小缓存，大文件需要流式IO。

### ZeroTier

```cpp
using JsonCallback = std::function<void(const JsonResult &)>;
void getZTInfo(QObject *context, JsonCallback completed);
void getZTPeers(QObject *context, JsonCallback completed);
void getZTNetworks(QObject *context, JsonCallback completed);
void getZTMoons(QObject *context, JsonCallback completed);
void sendDataTo(const QString &networkId, const QString &memberId, const QByteArray &data,
                QObject *context, SendCallback completed);
void broadcastDataTo(const QString &networkId, const QByteArray &data,
                     QObject *context, SendCallback completed);
```

通过 `host->zeroTier()` 使用，分别复用宿主CLI的 `-j info/listpeers/listnetworks/listmoons`、平台路径、授权和超时逻辑，不触发提权。`JsonResult` 包含 `QJsonDocument document`、`QString error` 和 `ok()`：Info为对象，其余为数组，保留原始字段。

查询在GUI线程以有效GUI线程QObject上下文调用，回调异步在该线程执行；上下文销毁或卸载取消请求并释放回调。命令失败、超时、无效JSON及错误类型均返回错误。错误线程或空上下文被拒绝并记警告，不调用回调。`onDestroy()` 阶段查询已关闭，但仍可同步保存文件。

插件运行在宿主进程中，不提供权限隔离。SDK不提供任意命令或管理员脚本接口。语音、桌面共享、文件传输的开发条件和待补充公共能力见[插件能力评估](plugin-api-assessment.md)。

数据发送同样通过 `host->zeroTier()` 调用，要求 GUI 线程与有效上下文；销毁上下文或卸载取消尚未完成的查询。网络 ID 是 16 位十六进制，成员 ID 是 10 位。先读取本机指定网络的 IPv4，以 /24 的 `.1:18100` 为服务器，通过 `builtin/controller.members` 获取该网络成员；仅向已授权且有 IPv4 地址的成员发送。`sendDataTo` 选择一个成员，`broadcastDataTo` 向所有可发送成员逐个单播，并去重目标 IP。

插件自行连接服务器时使用 `<ztprotocol/NetworkAddress.h>` 的 `protocol::serverEndpoint(address)`：接受域名、IPv4、裸IPv6及 `host:port`／`[IPv6]:port`，未指定端口统一使用 `protocol::NetworkPort`（18100），非法地址／端口的结果 `valid()` 为false。独立端口偏好不能覆盖省略端口的默认值；UDP端口仍可由服务器协议协商。

发送采用 UDP 18100，包为 `ZTS1`，路由 ID 由宿主绑定为调用插件 ID，负载原样保留二进制。包总长最多 1232 字节，数据上限为 `1232 - 6 - 插件ID的ASCII字节数`。接收端插件需使用相同路由，并在成员设备上监听 UDP 18100；服务端宿主已经提供该监听与路由，客户端自定义业务由插件负责接收。`SendResult` 包含 `recipients/error/ok()`，成功仅代表本地 UDP 套接字已接收发送数据，不代表远端收到；需要可靠性时由业务协议提供确认、重传或另选 TCP。无匹配授权成员、无 IPv4、过大负载或查询失败均异步返回错误。UDP 工具不代替网络或业务层身份验证。

## 独立构建与兼容边界

`ZTGUI::Api` 是头文件SDK，提供C++17、Qt Quick/Qml 与 Network 依赖，没有API DLL。独立安装：

```powershell
cmake -S src/client/api -B cmake-build-api -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --install cmake-build-api --prefix dist/ZTGUI-api-v0.3
```

独立插件使用 `find_package(ZTGUIPluginApi 0.3 EXACT CONFIG REQUIRED)` 并链接 `ZTGUI::Api`，只需安装后的SDK和匹配工具链，不需要宿主源码。API常量及包版本从根 `VERSION` 前两段生成；JSON使用 `{"apiVersion":"@ZTGUI_API_VERSION@"}`，不硬编码另一份版本。

应用版本为 `0.3.3`，API代际从 `VERSION` 派生为 `0.3`。此前重构改变命名空间及生命周期签名，旧插件须使用API 0.3重新编译。入口IID为 `vusiif.ztgui.api.PluginInterface/2.0`，生命周期IID为 `vusiif.ztgui.api.PluginLifecycleInterface/2.0`；旧IID不会实例化，API 0.2也不再兼容。本次补丁版本更新不改变 API 代际，程序版本由开发者指定。

统一使用动态Qt，宿主与插件须匹配Qt、编译器、架构、运行库及构建模式。Qt类型和std::function属于进程内C++接口，不承诺跨工具链ABI或跨进程协议。

启动扫描程序旁 `plugins/` 的直接子文件，按文件名排序，先检查IID和API数字，再实例化并注册。Windows为 `ZTGUI.exe` 同级plugins；macOS为 `ZTGUI.app/Contents/MacOS/plugins/`。缺失目录正常启动，其他库忽略，失败记日志并继续。更新插件退出后替换并重启，运行依赖须与发行包共同部署。

最小独立插件、完整构建及部署流程见[插件开发技能](../.agent/skills/ztgui-plugin-dev/SKILL.md)。插件自身文档由其项目的docs目录维护。

## 验证

回归测试覆盖注册与API校验、菜单排序、懒创建/缓存、页面所有权、失败重试、重入与线程检查、退出/进入顺序、协作取消、队列跳过、GUI心跳、重复关闭、不可取消步骤与库销毁顺序、文件路径和二进制内容、JSON错误与上下文取消。旧IID拒绝测试确保旧库不会在新接口下被误实例化。测试辅助程序不进入发行包。

## 插件日志与协议工具

通过 `HostServices::logs()` 获取借用的 `PluginLogger`，调用 `write(LogLevel,message)` 显式双写主程序日志和 `plugins-data/<PluginInfo.name>/logs/YYMMDD-N.log`。级别为 Debug/Info/Warning/Error，UTF-8 记录包含时间、级别和插件 ID；支持后台线程，生命周期与其他宿主工具相同。返回 `LogResult::hostError/pluginError`，一处失败仍尝试另一处；失败不递归写 Qt 日志。普通 `qInfo/qWarning` 仍只写宿主日志。新增服务放在原宿主服务虚函数之后，并以旧布局编译的插件验证了原文件/查询入口。

SDK 安装 `ztprotocol/` 的共享公开头文件，`ZTGUI::Protocol` 依赖 Qt Core/Network，提供路由编解码、/24 服务器地址推导与网络数据发送工具。协议版本独立于 SDK 版本；服务端插件使用独立 SDK 与 IID，详见[服务端 API](server-api.md)。
