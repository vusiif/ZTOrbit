# 服务端插件 API 与传输协议

宿主是 Java 25 + Netty 的 `zt-server`。公开 SDK 在 `src/server/api/`（Maven 模块 `ztserver-api`），实现位于 `src/server/main/`（模块 `ztserver`），Java 插件默认在 `src/plugins/<name>/server/`。服务端程序版本由客户端侧维护的根 `SERVER_VERSION` 指定，当前程序 `0.2.1-beta`、API `0.2-beta`；Java 插件必须精确匹配完整 API 版本（含预发布后缀）。客户端与服务端版本相互独立。

旧 C++ 服务端与 C++ `.ztplugin` 已归档到 `src/server-old/`，不再由宿主加载。

## 独立 SDK

发布构建生成 `build/dist/ZTServer-api-v0.2-beta.zip`，目录本身是 Maven 仓库根：

```text
ZTServer-api-v0.2-beta/
  vusiif/ztgui/ztserver-api/0.2-beta/
    ztserver-api-0.2-beta.jar
    ztserver-api-0.2-beta-sources.jar
    ztserver-api-0.2-beta-javadoc.jar
    ztserver-api-0.2-beta.pom
  sample/           最小插件项目
  sdk.json          API/程序版本、JDK 要求、输入指纹
  README.md         SDK 说明与独立消费示例
```

插件项目使用 JDK 25 / Maven 3.9+，依赖：

```xml
<dependency>
  <groupId>vusiif.ztgui</groupId>
  <artifactId>ztserver-api</artifactId>
  <version>0.2-beta</version>
  <scope>provided</scope>
</dependency>
```

`provided` 表示运行时由宿主提供 API、Jackson 与 Netty，插件 JAR 不应 shade 这些依赖。SDK 的 `sample/pom.xml` 通过 `file:` 仓库直接引用解压目录，可在临时本地仓库验证：

```bash
mvn -f build/dist/ZTServer-api-v0.2-beta/sample/pom.xml package
```

## 插件格式与加载

插件是 `plugins/*.jar`（直接子文件、非符号链接、按文件名排序）。JAR 内 `META-INF/ztserver-plugin.json`：

```json
{
  "formatVersion": 1,
  "host": "server",
  "id": "voice-chat",
  "name": "语音聊天",
  "version": "0.1.0",
  "apiVersion": "0.2-beta",
  "mainClass": "vusiif.ztgui.voicechat.VoiceChatPlugin"
}
```

校验规则：`formatVersion==1`、`host=="server"`、ID 匹配 `[A-Za-z0-9_-]{1,128}` 且大小写不敏感地不等于 `builtin`、显示名去空白非空、版本非空、`apiVersion` 精确等于宿主 API（当前 `0.2-beta`，含预发布后缀）、`mainClass` 非空；JAR 最大 256 MiB，manifest 最大 64 KiB。仅接受直接子目录中的 `.jar`，拒绝符号链接和非普通文件；发现 `.ztplugin` 时记录 `unsupported package, use .jar`。

宿主用 `URLClassLoader`（parent 为宿主 classloader）实例化 mainClass（无参构造），实例必须实现 `ServerPlugin`，`info()` 的 `id/name/version` 必须与 manifest 完全一致。重复 ID 或重复显示名（大小写不敏感）拒绝后加载的插件。加载失败关闭 classloader；启动失败保留已加载对象但不 ready，停止时仍调用 `onStop` 并关闭 classloader。只在启动时扫描，增减插件后重启。

## API 签名

包名 `vusiif.ztgui.server.api`：

```java
public interface ServerPlugin {
    PluginInfo info();
    default StartTask onStart(HostServices host, CancellationToken cancellation) { return null; }
    default void onTcpPacket(long connectionId, byte[] payload) {}
    default void onUdpPacket(InetSocketAddress endpoint, byte[] payload) {}
    default void onConnectionClosed(long connectionId) {}
    default void onStop() {}
}

public record PluginInfo(String id, String name, String version) {}
public record StartResult(boolean success, String message) {}
@FunctionalInterface public interface StartTask { StartResult start() throws Exception; }
@FunctionalInterface public interface Cancellable { void cancel(); }

public interface HostServices {
    PluginFiles files();
    PluginLogger logs();
    ZeroTierQueries zeroTier();
    ScheduledExecutorService scheduler();
    int port();
    InetAddress peerAddress(long connectionId);
    boolean sendTcp(long connectionId, byte[] payload);
    boolean sendUdp(InetSocketAddress endpoint, byte[] payload);
    void disconnect(long connectionId);
    long queuedTcpBytes(long connectionId);
    Cancellable sendDataTo(String networkId, String memberId, byte[] data, SendCallback completed);
    Cancellable broadcastDataTo(String networkId, List<String> memberIds, byte[] data, SendCallback completed);
    Cancellable broadcastDataBalancedTo(String networkId, List<String> memberIds, byte[] data, SendCallback completed);
    Cancellable getNetworkRelays(String networkId, RelayCallback completed);
    NetworkSubscription subscribeNetworkData(String networkId, NetworkCallback received);
}

public interface ZeroTierQueries {
    Cancellable query(String operation, JsonNode parameters, QueryCallback completed);
}
```

结果与回调：`QueryResult(JsonNode document, String error)`、`FileResult(byte[] data, String error)`、`LogResult(String hostError, String pluginError)`、`SendResult(int recipients, String error, List<String> failedMembers)`、`RelayInfo(...)`、`RelayResult(List<RelayInfo> relays, String error)`、`NetworkMessage(String networkId, String senderId, byte[] data)`；`QueryCallback`/`SendCallback`/`RelayCallback`/`NetworkCallback` 均在宿主事件线程执行，异常被捕获并记录。`ApiVersion.value()` 读取过滤后的 `api.properties`，`matches(String)` 精确比较完整 API 版本（含预发布后缀）。

## 线程与生命周期

- `onStart`、`onTcpPacket`、`onUdpPacket`、`onConnectionClosed` 及所有查询/发送/网络回调在单一宿主事件线程执行；插件必须快速返回。
- `onStart` 返回的 `StartTask` 在虚拟线程执行；返回 `StartResult.ok()` 后插件才 ready，失败则记录并保持不 ready。
- `files()`/`logs()` 线程安全；`sendTcp/sendUdp/disconnect/queuedTcpBytes/scheduler()` 线程安全；其余方法限事件线程。
- `onStop` 在取消查询/订阅/初始化任务并等待任务后调用；插件必须停止并等待自己的线程与定时器。
- `scheduler()` 返回宿主事件线程调度器；`onStop` 中取消所有任务。

停止顺序：标记 stopping → 停止维护定时器与网络轮询 → 关闭 TCP/UDP 监听、取消 ZeroTier 查询 → 插件不 ready 并取消初始化/查询/订阅/发送回调 → 关闭现存连接并回调 `onConnectionClosed` → 等待初始化任务、`onStop`、关闭 classloader → 关闭 Netty event loop，退出 0。

## 文件与日志

插件数据目录为启动目录的 `plugins-data/<插件显示名>/`，插件日志在 `plugins-data/<显示名>/logs/YYMMDD-N.log`。文件名/目录名校验拒绝空、`.`、`..`、结尾点/空格、`/\\:*?"<>|`、控制字符、Windows 设备名和符号链接；`plugins-data` 必须是普通目录。相对路径拒绝绝对路径、盘符/冒号、NUL、空段、`.`/`..`、结尾点/空格，并逐段检查符号链接与类型；写入自动创建父目录并原子替换。

插件 `logs().write(level,message)` 同时写宿主日志（行带 `[plugin-id]`）和插件日志，返回 `hostError/pluginError`；级别为 DEBUG/INFO/WARNING/ERROR，宿主日志级别名 WARN。宿主日志为启动目录 `logs/YYMMDD-N.log`，进程生命周期内固定文件；启动时写入 `服务启动 zt-server v<version>` 与 Java 运行时版本。

## ZTS1 外层协议

| 字节 | 内容 |
| --- | --- |
| 0–3 | ASCII `ZTS1` |
| 4 | 协议版本 1 |
| 5 | 路由 ID 长度，1–128 |
| 6 起 | ASCII 路由 ID，随后为不透明插件负载 |

ID 字符集 `[A-Za-z0-9_-]{1,128}`，`builtin` 为保留路由。TCP 在包前加 4 字节大端长度，整包最大 1 MiB；支持拆包/粘包；非法帧断开。UDP 一个数据报一个包，最大 1232 字节；非法/超长丢弃。宿主按 ID 精确分发（插件查找大小写不敏感），不解析业务负载。未知或未就绪路由的 TCP 包收到 `builtin` JSON，UDP 丢弃。

每连接每秒最多 60 个控制包，30 秒无有效包断开；最多 256 个同时连接；发送积压超过 2 MiB 断开；读缓冲上限 `MaxTcpBytes + 4`；发送帧负载上限 `MaxTcpBytes - 6 - routeLen`。

## `builtin` TCP JSON

请求 `{"v":1,"id":<number>,"op":"...","data":{...}}`。非对象、`v != 1`、缺少 `id` 时断开连接；`op` 以 `network.` 开头的请求交给网络中转，其余交给只读查询。响应：

```json
{"v":1,"id":<原值>,"ok":true,"data":...}
{"v":1,"id":<原值>,"ok":false,"error":"..."}
```

未知/未就绪插件路由的错误固定为 `{"v":1,"ok":false,"error":"unknown_or_unready_plugin","plugin":"<route>"}`（无 `id` 字段，保持旧行为）。

## 内置只读查询

| op | 结果 |
| --- | --- |
| `info` | 本机状态对象（`/status`） |
| `peers` | peers 数组（`/peer`） |
| `networks` | 已加入网络数组（`/network`） |
| `moons` | Moon 数组（`/moon`） |
| `controller.networks` | 控制器网络列表（`/controller/network`） |
| `controller.members` | 指定网络成员，返回 `nodeId/name/authorized/ipAssignments` |

`controller.members` 的 `networkId` 必须匹配 `^[0-9a-f]{16}$`；最多 10000 个成员，4 并发、15 秒总期限；任一成员失败即返回错误；空列表返回 `[]`。宿主直连 `http://127.0.0.1:9993`，从 `/var/lib/zerotier-one/authtoken.secret` 读取 `X-ZT1-Auth`，禁用代理、不跟随重定向、10 秒超时、响应上限 4 MiB，不输出 token。最多 64 个待处理查询，超出返回 `Too many pending queries`。测试/运维可通过环境变量覆盖服务地址和 token：`ZTSERVER_ZT_API`、`ZTSERVER_ZT_TOKEN`（不写入公开 API）。

## 网络中转

完整保留旧 `src/shared/network/NetworkTransport.cpp` 行为，Java 实现在 `src/server/main/src/main/java/vusiif/ztgui/server/main/network/`。每网络使用 16 位 hex 网络 ID/10 位成员 ID、本机首个 IPv4、控制器 `.1`（/24）；测试可用 `transportController/transportControllerPort` 覆盖。服务器每网络 UDP socket 绑定本机 IP + 端口 0；客户端接收端口 18101，服务器接收端口 18100。

每 2 秒刷新 `controller.members`（20 秒期限）、`network.relays`、中转探活（1 秒超时），清理 30 秒 binding 和 reserved；成员缓存 30 秒、端点 10 秒、binding 30 秒。`network.relays` 验证来源 IP 是网络成员，返回 `relays.json` 配置、主 `.1:18100` 与 10 秒内活动接收端点。`network.register` 只接受 18100/18101，同一节点优先客户端接收器，10 秒内旧端点不覆盖。`network.forward` 校验插件 ID、16 字节消息 ID、base64 负载上限、目标 ≤4096 并去重，通过每网络 socket 发送 `ZTD1` 到目标端点（默认 18101）。

`sendDataTo`/`broadcastDataTo` 走单中转，`broadcastDataBalancedTo` 按 RTT/负载稳定分配并在失败时重试/直发；结果含 `recipients/error/failedMembers`。接收 `ZTD1` 时验证目标、已授权发送者、来源 IP 或已配置中转，30 秒去重、最多 8192 条，通过 `subscribeNetworkData` 分发；`ZTD1` 不再交给 `onUdpPacket`。`relays.json` 启动读取，格式为 `{"<networkId>":[{"nodeId","moonId","virtualIp","port"},...]}`，非法内容忽略并告警。详细行为与端口规则见[多中转通信](network-relays.md)。

## 安装与卸载

宿主 CLI 不再包含 `--install/-i`、`--uninstall/-u`、`--moon/-m`、`--ip`、`--force`、`--yes`、`--dry-run`。安装/卸载/Moon 由发行包内的独立 `zt-installer.sh` 负责，旧命令映射见 [ZTServer 部署说明](../src/server/DEPLOYMENT.md)。

## 相关文档

- [服务端实现与构建](../src/server/README.md)
- [服务端 API 变更说明](../CHANGELOG.md)
- [Java 语音插件](../src/plugins/voice-chat/server/docs/voice-chat.md)
- [网络中转行为](network-relays.md)
- [插件运行库分离](plugin-runtime-separation.md)
