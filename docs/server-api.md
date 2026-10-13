# 服务端插件 API 与传输协议

宿主是 `zt-server`，公开 SDK 在 `src/server/api/`，实现位于 `src/server/main/`，插件默认在 `src/plugins/<name>/server/`。客户端和服务端版本均由根 `VERSION` 派生，当前程序0.3.3 / SDK0.3；相同数字不代表两端动态库可混载。

## 独立 SDK 与插件

```bash
cmake -S src/server/api -B cmake-build-server-api -DCMAKE_INSTALL_LIBDIR=lib
cmake --install cmake-build-server-api --prefix "$PWD/dist/ZTServer-api-v0.3"
```

第三方只需解压 SDK、匹配的动态 Qt6、编译器和运行库，不需要宿主源码。插件项目：

```cmake
cmake_minimum_required(VERSION 3.21.1)
project(example LANGUAGES CXX)
set(CMAKE_AUTOMOC ON)
find_package(ZTServerPluginApi 0.3 EXACT CONFIG REQUIRED)
configure_file(plugin.json.in "${CMAKE_CURRENT_BINARY_DIR}/plugin.json" @ONLY)
qt_add_plugin(example SHARED CLASS_NAME Example)
target_sources(example PRIVATE Plugin.cpp)
target_include_directories(example PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(example PRIVATE ZTServer::Api)
```

`plugin.json.in` 内容为 `{"apiVersion":"@ZTSERVER_API_VERSION@"}`。通过 `-DCMAKE_PREFIX_PATH=/path/to/sdk` 配置。`Plugin.cpp` 最小实现：

```cpp
#include <ztserver/PluginInterface.h>
using namespace vusiif::ztgui;
class Example final : public QObject, public server::api::PluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ZTSERVER_PLUGIN_IID FILE "plugin.json")
    Q_INTERFACES(vusiif::ztgui::server::api::PluginInterface)
    server::api::HostServices *host = nullptr;
public:
    server::api::PluginInfo info() const override { return {"example.echo","示例","1.0"}; }
    server::api::StartTask onStart(server::api::HostServices &services, const util::CancellationToken &) override {
        host = &services;
        host->logs().write(util::LogLevel::Info,"示例已启动");
        return {};
    }
    void onTcpPacket(server::api::ConnectionId connection, const QByteArray &payload) override {
        host->sendTcp(connection,payload);
    }
};
#include "Plugin.moc"
```

插件 ID 匹配 `[a-z0-9][a-z0-9._-]{0,127}`，`builtin` 保留；重复 ID 不会覆盖先加载的插件。IID 为 `vusiif.ztgui.server.api.PluginInterface/1.0`，元数据和接口都精确匹配 API 两段版本。插件共享宿主进程，必须匹配 Linux 架构、Qt、工具链与运行库，不承诺跨工具链 ABI。

## 生命周期与工具

`onStart` 在事件线程快速返回，空任务立即就绪；返回的任务在后台执行，通过取消令牌协作退出，成功后才接收包。收包和连接关闭通知在事件线程执行，耗时业务使用插件自己的工作线程。停止顺序为停止收发、取消查询与初始化、等待任务、`onStop`、销毁根对象、释放动态库。插件应在 `onStop` 停止并等待自己的线程、定时器和长期任务。

`HostServices` 借用指针有效到 `onStop` 返回。`sendTcp` 使用宿主连接 ID，`sendUdp` 使用地址与端口端点，二者自动封装当前插件的路由 ID；插件不持有宿主 socket。`queuedTcpBytes` 返回待发送字节数，插件可设置更严格的积压限制。查询和网络方法只在事件线程调用；文件和日志支持后台线程。宿主最多256个连接，每连接每秒60个控制包、30秒无控制包断开，输出队列上限2 MiB。

`files()` 的相对路径绑定到启动目录的 `plugins-data/<插件名称>/`；禁止绝对路径、`..`、链接和无效目录名，写入采用原子替换。名称应稳定、唯一，同名共用数据目录。`logs().write(LogLevel,message)` 同时写宿主日志和插件自己的 `logs/YYMMDD-N.log`，返回 `hostError/pluginError`；四种级别为 Debug、Info、Warning、Error，文件包含时间戳、级别和插件 ID。Qt 的普通日志写宿主文件，双写须显式调用工具接口。

## 外层协议

公共 `<ztprotocol/WireProtocol.h>` 导出 `encode/decode/frame/takeFrame`，两端 SDK 分别提供 `ZTGUI::Protocol`、`ZTServer::Protocol`（仅 Qt Core 依赖）。协议版本独立于 API 版本。

| 字节 | 内容 |
| --- | --- |
| 0–3 | ASCII `ZTS1` |
| 4 | 协议版本1 |
| 5 | 插件 ID 长度，1–128 |
| 6起 | ASCII 插件 ID，随后为不透明业务负载 |

TCP 在整个包前加四字节大端长度，包体最大1 MiB；UDP 一个包一个数据报，包头加负载最大1232字节。宿主按 ID 精确分发，不解析插件负载，不替插件选择接收对象。未知或未就绪路由的 TCP 包收到 `builtin` 错误，UDP 丢弃；非法 TCP 帧断开，非法 UDP 丢弃。内层协议由插件定义，旧独立语音协议不能直接连接新宿主。

## 内置只读查询

通过 TCP、路由 `builtin` 发送 UTF-8 JSON：`{"v":1,"id":1,"op":"controller.members","data":{"networkId":"0123456789000000"}}`。响应含对应 `id`、`ok`，成功为 `data`，失败为 `error`。仅支持下列查询，不提供远程安装、卸载或授权操作：

| op | 内容 |
| --- | --- |
| `info` | 本机状态对象 |
| `peers` | 本机发现的 peers 数组 |
| `networks` | 本机已加入网络数组 |
| `moons` | 本机 Moon 数组 |
| `controller.networks` | 本机控制器的网络列表 |
| `controller.members` | 指定网络成员，返回 `nodeId/name/authorized/ipAssignments` |

插件使用 `zeroTier().query(op,parameters,context,callback)` 获取相同结果。上下文必须在事件线程；销毁上下文或停止插件取消请求。宿主直连 `127.0.0.1:9993`，读取本机认证文件并禁用代理，不返回认证文件内容。单次HTTP请求10秒超时，成员详情最多4个并行请求、15秒总期限；任何成员查询失败时返回错误，不返回静默缺失的成员列表。

## 按网络与成员发送数据

`HostServices::sendDataTo(networkId,memberId,data,context,callback)` 和 `broadcastDataTo(networkId,data,context,callback)` 在事件线程异步查询本机控制器的指定网络成员，随后通过 UDP 18100 发送给已授权成员的 IPv4。单成员接口要求 10 位成员 ID；广播逐个单播并去重 IP。数据包装为调用插件路由的 ZTS1 包，包总长不超过 1232 字节。回调类型为 `util::SendCallback`，结果含 `recipients/error/ok()`，成功只表示本地发送，不保证交付。接收方需监听 UDP 18100 并按同一插件路由接收；客户端接收由业务插件实现。上下文销毁或 `onStop` 取消未完成查询。具体限制见[客户端 API](plugin-api.md)。

客户端网络成员页约定掩码 255.255.255.0；从本机分配的 IPv4 推导 `.1:18100`，使用上述 `controller.members` 请求并每 2 秒轮询。该 `.1` 节点需运行新宿主，且本机 ZeroTier 控制器应管理对应网络；其他控制器部署需自行调整部署地址。该只读接口沿用已有网络访问控制，不增加远程管理权限。
