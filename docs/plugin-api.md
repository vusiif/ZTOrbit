# 插件 API（开发代际 0.5-alpha）

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
| `vusiif::ztgui::voice_chat` | 语音客户端与 C++ 共享协议（Java 服务端使用 `vusiif.ztgui.voicechat`） | `src/plugins/voice-chat/` |

头文件路径仍使用 `<ztgui/...>`，CMake 目标仍为 `ZTGUI::Api`。不提供旧命名空间别名。宿主的 `PluginRegistry`、`PluginLoader` 和 `HostServicesImpl` 不属于公开 SDK，插件不能包含其实现头文件。

## QML页面与二进制边界

页面可通过 `<ztgui/PluginPage.h>` 的 `createQmlPage(engine,parent,source,initialProperties)` 从插件自己的资源创建。C++控制器通过required属性注入，由页面持有；页面先于工厂和DLL销毁，宿主清理引擎组件缓存后才卸载库。插件不能注册DLL所有的全局QML类型或单例，也不能依赖宿主内部QML模块。可使用Qt Quick Controls并继承应用窗口palette。

客户端应用与 API 均由根 `VERSION` 派生（当前 `0.5.1-alpha` → API `0.5-alpha`），由新入口／生命周期IID `/3.0` 区分页面ABI。旧 `/1.0`、`/2.0` DLL在实例化前拒绝，必须迁移页面工厂并重编。SDK依赖Qt Quick/Qml，已移除Widgets依赖；应用保留Widgets仅供Windows原生托盘菜单后备实现。

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

清理顺序为：请求取消 → 确认任务结束 → 退出当前插件菜单 → 完成业务异步停止 → 销毁QML页面、控制器和工厂 → 清理引擎组件缓存 → 取消服务查询 → `onDestroy()` → 销毁插件根对象并卸载库。初始化失败、取消或未执行时也会清理生命周期，`onDestroy()` 应允许资源尚未准备或仅部分准备。

取消不能强行打断不可取消的第三方调用，耗时IO应有自己的超时或取消方式。异步停止完成前不能销毁仍使用业务线程的页面或卸载 DLL。直接析构属于紧急兜底，不会强行卸载未停止的进程内 DLL。长期业务通过独立 `PluginStopInterface/1.0` 提供异步停止；见后面的服务扩展。

宿主内部 `PluginLoader::requestShutdown()` 非阻塞，`shutdownProgress(QStringList)` 报告待结束的插件名，`shutdownFinished()` 表示初始化、业务停止、线程池及实际卸载检查已结束。调用者须保持加载器和注册表存活至完成，再执行析构清理。内部单插件 `unload(pluginId, registry)` 也快速返回受理结果，以 unloadFinished 报告真实完成，整体停机期间拒绝扫描和卸载。插件管理界面显示已安装包、依赖、服务和运行状态；没有目录监听。

## 数据结构与约束

| 类型 | 字段/签名 | 约定 |
| --- | --- | --- |
| `PluginInfo` | `id`, `name`, `version`, `apiMajor`, `apiMinor` | ID全局唯一；名称与版本非空，API字段默认从SDK生成常量取得 |
| `MenuContribution` | `id`, `title`, `icon`, `toolTip`, `order`, `createPage` | 本地ID唯一，标题非空，工厂必填；默认order为100 |
| `PluginDefinition` | `PluginInfo info`, `QList<MenuContribution> menus` | 允许无菜单的服务插件 |
| `RegisteredMenu` | `id`, `pluginId`, `localId`及显示字段 | 全局ID为 `pluginId/menuId` |
| `Result` | `Error error`, `QString message`, `ok()` | 错误码可供程序判断，message供展示或日志 |
| `PageResult` | `QQuickItem *page`, `Result result` | 失败时page为空 |
| `PageFactory` | `std::function<QQuickItem *(QQmlEngine *engine, QQuickItem *parent)>` | 在GUI线程返回新建、QObject parent与parentItem均为parent的可视项 |

插件 ID 匹配 `[A-Za-z0-9_-]{1,128}` 且冲突不区分大小写；本地菜单 ID 匹配 `[a-z0-9][a-z0-9._-]{0,127}`。`builtin` 为保留插件ID，内置ZeroTier始终位于首项；插件菜单按order再按全局ID排序。

注册不创建页面，首次访问才调用工厂，一个菜单缓存一个页面。切换菜单保留输入及滚动状态；缓存页面销毁后可重建。工厂不能返回父项、借用已有可视项或与其他菜单共用页面，不能跨宿主复用。工厂失败保留原页面和选中菜单，下次访问可重试。长期业务先通过独立异步停止接口结束，页面析构再做有界清理，随后才释放工厂捕获和插件库。

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
void broadcastDataTo(const QString &networkId, const QStringList &memberIds, const QByteArray &data,
                     QObject *context, SendCallback completed);
```

通过 `host->zeroTier()` 使用，分别复用宿主CLI的 `-j info/listpeers/listnetworks/listmoons`、平台路径、授权和超时逻辑，不触发提权。`JsonResult` 包含 `QJsonDocument document`、`QString error` 和 `ok()`：Info为对象，其余为数组，保留原始字段。

查询在GUI线程以有效GUI线程QObject上下文调用，回调异步在该线程执行；上下文销毁或卸载取消请求并释放回调。命令失败、超时、无效JSON及错误类型均返回错误。错误线程或空上下文被拒绝并记警告，不调用回调。`onDestroy()` 阶段查询已关闭，但仍可同步保存文件。

当前插件运行在宿主进程中，不提供权限隔离。SDK不提供任意命令或管理员脚本接口。独立服务、异步停止和媒体进程隔离已实现，当前契约见[插件服务](plugin-services.md)。文件传输、屏幕共享业务和 GPU 跨进程共享不在本轮范围。

数据接口由宿主绑定调用插件 ID，接收只分发给本机同 ID 插件；跨插件依赖仅允许通过认证 v3 清单和宿主管理的公开服务，禁止引用私有头／实现 DLL 或私自查找对象。ZeroTier 查询均为只读，不开放网络加入、授权、配置修改等操作。原生插件不构成权限沙箱。

```cpp
void broadcastDataBalancedTo(const QString &networkId, const QStringList &memberIds,
                             const QByteArray &data, QObject *context, SendCallback completed);
void getNetworkRelays(const QString &networkId, QObject *context, RelayCallback completed);
NetworkSubscription *subscribeNetworkData(const QString &networkId, QObject *context, NetworkCallback received);
```

`broadcastDataTo` 固定走指定网络 `.1:18100` 的 ZTServer；`broadcastDataBalancedTo` 在控制端登记的多中转间低延迟优先、按目标成员稳定分配。两者目标 ID 列表去重，空列表零发送；`sendDataTo` 复用单中转路径。中转不可用时按有效已授权成员地址直发，不能解析地址则返回错误，不假定数据已送达。

`RelayResult` 包含中转节点 ID、Moon ID、虚拟 IP、服务端口、最近 10 次成功 RTT 的平均、上次 RTT、采样时间、可用状态与负载；延迟未知为 -1。`SendResult` 包含排入队列的成员数、失败成员 ID 和错误，不代表远端交付确认。`NetworkMessage` 提供网络 ID、发送成员 ID、二进制数据。订阅句柄由上下文持有，调用 `cancel()` 停止接收；上下文销毁或卸载自动取消，回调均在宿主事件线程执行。

共享交付包仍封装 ZTS1，总长上限 1232，负载上限由 `<ztprotocol/NetworkDelivery.h>` 的 `maxNetworkDataBytes(pluginId)` 返回，即 `1232-52-插件 ID ASCII 字节数`。本次不支持分片、重传或顺序保证。客户端宿主在本机 ZeroTier IPv4 的 UDP 18101 接收，服务端使用 18100；网络、发送与目标成员及消息 ID 由宿主校验，同一消息 30 秒内去重。部署与中转配置见[多中转通信](network-relays.md)。

插件自行连接服务器时使用 `<ztprotocol/NetworkAddress.h>` 的 `protocol::serverEndpoint(address)`：接受域名、IPv4、裸IPv6及 `host:port`／`[IPv6]:port`，未指定端口统一使用 `protocol::NetworkPort`（18100），非法地址／端口的结果 `valid()` 为false。独立端口偏好不能覆盖省略端口的默认值；UDP端口仍可由服务器协议协商。


## 独立构建与兼容边界

`ZTGUI::Api` 是头文件SDK，提供C++17、Qt Quick/Qml 与 Network 依赖，没有API DLL。独立安装：

```powershell
cmake -S src/client/api -B build/cmake-build-api -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --build build/cmake-build-api --config Release --target ztplugin_pack
cmake --install build/cmake-build-api --config Release --prefix build/dist/ZTGUI-api-v0.5-alpha
```

独立插件使用 `find_package(ZTGUIPluginApi 0.5 EXACT CONFIG REQUIRED)` 并链接 `ZTGUI::Api`，只需安装后的SDK和匹配工具链，不需要宿主源码。CMake 包版本使用数字基版本 `0.5`；`ZTGUI_API_VERSION` 与插件 JSON 使用完整 API `0.5-alpha`。JSON 使用 `{"apiVersion":"@ZTGUI_API_VERSION@"}`，不硬编码另一份版本。

客户端应用版本为 `0.5.1-alpha`，API 由 `VERSION` 前两段数字和同一预发布后缀派生为 `0.5-alpha`。API 0.4 及更早插件须使用 0.5 SDK 重新编译。入口IID为 `vusiif.ztgui.api.PluginInterface/3.0`，生命周期IID为 `vusiif.ztgui.api.PluginLifecycleInterface/3.0`；旧IID不会实例化，API 0.2也不再兼容。本次核心代际升级由开发者明确指定；IID不因版本数字变化而机械调整。

统一使用动态Qt，宿主与插件须匹配Qt、编译器、架构、运行库及构建模式。Qt类型和std::function属于进程内C++接口，不承诺跨工具链ABI或跨进程协议。

启动后台扫描程序旁 `plugins/` 的直接子文件，先完整认证、校验 ID／API／运行契约及全部传递依赖，再按依赖顺序执行入口；工作进程在握手就绪后才发布服务。Windows为 `ZTGUI.exe` 同级plugins；macOS为 `ZTGUI.app/Contents/MacOS/plugins/`。缺失目录正常启动，其他库忽略，失败记日志并继续。客户端可在完成异步卸载后替换业务包并重载；同名不兼容的运行依赖需要重启。

最小独立插件、完整构建及部署流程见[插件开发技能](../.agents/skills/ztgui-plugin-dev/SKILL.md)。插件自身文档由其项目的docs目录维护。

## 验证

回归测试覆盖注册与API校验、菜单排序、懒创建/缓存、页面所有权、失败重试、重入与线程检查、退出/进入顺序、协作取消、队列跳过、GUI心跳、重复关闭、不可取消步骤与库销毁顺序、文件路径和二进制内容、JSON错误与上下文取消。旧IID拒绝测试确保旧库不会在新接口下被误实例化。测试辅助程序不进入发行包。

## 插件日志与协议工具

通过 `HostServices::logs()` 获取借用的 `PluginLogger`，调用 `write(LogLevel,message)` 显式双写主程序日志和 `plugins-data/<PluginInfo.name>/logs/YYMMDD-N.log`。级别为 Debug/Info/Warning/Error，UTF-8 记录包含时间、级别和插件 ID；支持后台线程，生命周期与其他宿主工具相同。返回 `LogResult::hostError/pluginError`，一处失败仍尝试另一处；失败不递归写 Qt 日志。普通 `qInfo/qWarning` 仍只写宿主日志。既有日志服务保持原布局；本轮通用服务通过独立 ServiceAccess 扩展查询，未在 HostServices 原虚表上追加入口。

SDK 安装 `ztprotocol/` 的共享公开头文件，`ZTGUI::Protocol` 依赖 Qt Core/Network，提供路由编解码、/24 服务器地址推导与网络数据发送工具。协议版本独立于 SDK 版本；服务端 Java 插件使用独立 SDK 与 JAR manifest 的 `apiVersion`（不使用 Qt IID），详见[服务端 API](server-api.md)。

## 加密包与运行时管理

客户端入口／生命周期 IID 为 `vusiif.ztgui.api.PluginInterface/3.0` 和 `vusiif.ztgui.api.PluginLifecycleInterface/3.0`。API 版本仍从 `VERSION` 前两段数字和同一预发布后缀派生，旧 ABI 必须重编。插件 `id` 匹配 `[A-Za-z0-9_-]{1,128}`，保留字 `builtin` 不可用，ID 冲突不区分大小写；`name` 为可中文的显示名，菜单 ID 独立于插件 ID。显示名对应既有数据目录，因此也禁止冲突，避免两个插件共享文件。

SDK 包含 `bin/ztplugin_pack` 和 `ztgui_package_plugin()` CMake 函数，编译后生成 `.ztplugin`；正式宿主仅扫描该后缀。包基本信息可读，包含宿主类型、版本、IID、平台、架构与 Qt/编译器兼容信息。宿主先检查信息、全包认证、逐文件 SHA-256、路径与运行契约，再按块提取到私有目录。加载后核对插件定义；业务目录在实际卸载成功后清理，旧普通插件的非 Qt 私有运行库兼容路径保持到退出；额外 Qt 模块／后端必须使用 v3 独立 worker，媒体目录不会进入宿主搜索路径。共享格式密钥只用于封装、防随意查看，不能证明发行者身份；不包含用户密钥或授权发行体系。

启动和运行时加载共用 onCreate；初始化准备期间菜单禁用，失败也配对清理。运行时卸载先切回 ZeroTier、调用 onExitedTo，再取消任务和请求，异步等待初始化结束，释放页面／工厂／回调后调用 onDestroy，最后卸载动态库。卸载失败保留句柄与临时库，不报告成功。卸载保留包和数据，按 ID 持久化禁用；手动加载成功恢复启动自动加载。

## 旧格式兼容与收集工具

SDK helper 新增 RUNTIME_QT_MODULES、RUNTIME_FILES、LICENSE_FILES 及显式 `<target>_package`；默认写 v2，宿主兼容读取 v1。Windows 媒体依赖随 v3 工作进程包交付；进程内插件不可覆盖宿主基础库或携带额外 Qt 模块／后端。普通插件维持 Qt 主次兼容。格式、许可证导出、冲突与卸载约束见[运行依赖分离](plugin-runtime-separation.md)。

## v3 服务与异步停止扩展

使用 `SERVICE_MANIFEST` 生成 ZTPK3，认证清单声明执行方式、前置插件版本范围和所需／提供的服务。普通旧包仍使用原入口；有服务声明的新包不能降级为 v2。原入口／生命周期 `/3.0` 不变；`PluginInfo` 和 `PluginDefinition` 布局不变，允许没有菜单。

`<ztgui/PluginServices.h>` 定义 `ServiceAccess`、`PluginServices`、`ServiceLease`、`ServiceResult`。在 onCreate 中通过 `dynamic_cast<util::ServiceAccess *>(&host)` 查询扩展，再 acquire 清单已声明的 provider/service/api。结果携带 JSON DTO、独立二进制 payload 和 error；call/subscribe/close 在 GUI 事件线程调用，回调绑定 QObject。租约绑定实例，不能长期缓存提供者裸指针；close 完成前不能释放仍执行中的消费者代码。

`<ztgui/PluginStop.h>` 定义独立 `PluginStopInterface/1.0`：`void requestStop(QObject *context, std::function<void(CreateResult)> completed)`。插件在 GUI 线程快速发起停止，异步结束会话与线程后完成回调；宿主再释放页面、工厂、回调和 DLL。失败时保留 DLL 及提供者并显示错误，最终 onDestroy 只做已停止状态的清理。加载器的 unload/shutdown 是受理请求，完成信号只在真实资源与模块检查结束后发出。

完整包语义、版本范围、事务、所有权、超时、进程 Job 和双通道 IPC 规范见[服务契约](plugin-services.md)；独立媒体 API 不属于宿主 SDK，宿主代码不包含媒体 API 或媒体实现。


## 无界面服务消费者

使用 `find_package(ZTGUIPluginApi 0.5 EXACT CONFIG REQUIRED COMPONENTS Services)` 与 `ZTGUI::Services` 取得仅 Core/Network 的服务契约，避免传递 Quick/QML。界面插件仍显式链接 `ZTGUI::Api`。协商的直接数据回调须持有 `ServiceLease::callbackGuard()` 返回的守卫，使宿主在回调结束前保留消费者 DLL。详见[媒体 API 与 Engine SDK](../src/plugins/media/client/docs/media.md)。
