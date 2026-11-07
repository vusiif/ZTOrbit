# 重置 ZeroTier 本地状态

设置页“ZeroTier”分区的“重置 ZeroTier 本地状态”会退出本机加入的全部 ZeroTier 网络，清除本机保存的网络配置、成员状态与缓存、Moon、自定义 planet 根节点文件，并移除 `local.conf` 中已过期的 TCP 回退中转设置，然后启动 ZeroTier 服务并验证本地管理 API。

**此操作只重启 ZeroTier 服务，不重启 Z2 应用。** 节点身份（`identity.secret` / `identity.public`）、本地 API 令牌（`authtoken.secret`）、`zerotier-one.port` 等运行凭据全部保留。

本文中的“成员”指**本机发现的 peer 状态/缓存**，不是远端控制器中的成员列表、授权或其他机器的配置。清除后 peer 会由 ZeroTier 重新发现，因此重置不会承诺固定的 peer 数量，也不会保留原连接。

## 数据目录

路径由现有 `dataDirectory()` / `systemHomeDirectory()` 解析，重置只使用真实系统目录：

| 平台 | 系统数据目录（重置目标） |
| --- | --- |
| Windows | `C:\ProgramData\ZeroTier\One` |
| macOS | `/Library/Application Support/ZeroTier/One` |
| Linux | `/var/lib/zerotier-one` |

macOS 的 `homeDirectory()` 是只含本地 API 凭据和 Moon 副本的临时会话缓存；重置不会把它当作系统数据目录，也不会清理它。它由现有会话刷新逻辑处理。

## 已验证的数据结构

ZeroTier One 的持久状态对象名称由 `service/OneService.cpp` 的 `nodeStateGetFunction` / `nodeStatePutFunction` 定义；Moon/planet 的加载与官方根节点回退规则见 `node/Topology.cpp`。参考：[ZeroTier 客户端配置](https://docs.zerotier.com/config/)、[OneService.cpp](https://github.com/zerotier/ZeroTierOne/blob/dev/service/OneService.cpp)、[Topology.cpp](https://github.com/zerotier/ZeroTierOne/blob/dev/node/Topology.cpp)。

| 对象 | 实际路径 | 说明 |
| --- | --- | --- |
| 已加入网络配置 | `networks.d/<16 位小写十六进制>.conf` | 服务启动时据此重新入网 |
| 网络本地设置 | `networks.d/<16 位小写十六进制>.local.conf` | 该网络的本地开关（如 `allowManaged`） |
| peer 状态/缓存 | `peers.d/<10 位小写十六进制>.peer` | 本机发现、缓存的 peer 路径与状态 |
| Moon | `moons.d/<16 位小写十六进制>.moon` | 已导入的 Moon |
| planet | `planet`（数据目录根下的普通文件） | 持久化的 planet world-state；存在自定义 planet 时替换官方根节点 |
| 身份 | `identity.secret`、`identity.public` | 节点身份 |
| 本地 API | `authtoken.secret`、`zerotier-one.port`、`metricstoken.secret` | 控制 API / 指标凭据与端口 |
| 本地配置 | `local.conf` | 节点级设置，重置只移除指定字段 |

### DELETE 白名单

只允许删除以下名称；实现中的精确正则以 `deletionWhitelist()` 为准：

| 正则（相对数据目录，`/` 分隔） | 删除原因 |
| --- | --- |
| `^networks\.d/[0-9a-f]{16}\.conf$` | 已加入网络的配置；退出全部网络 |
| `^networks\.d/[0-9a-f]{16}\.local\.conf$` | 随该网络成员关系一起失效的本地网络设置 |
| `^peers\.d/[0-9a-f]{10}\.peer$` | 本机 peer 状态/缓存 |
| `^moons\.d/[0-9a-f]{16}\.moon$` | 本机 Moon |
| `^planet$` | 根目录下的 planet world-state / 自定义 planet 覆盖文件 |

删除顺序固定为：`networks.d`、`peers.d`、`moons.d`、`planet`、`local.conf` 合并。每个候选路径都必须同时满足：相对路径、无 `..`/`.`/空段/反斜杠/盘符、匹配上表、真实路径位于数据目录内、目标与所有父目录都不是符号链接/junction/重解析点、目标是普通文件。**不会递归删除任何目录，不会对数据目录整体或状态目录做通配删除。**

`networks.d`、`peers.d`、`moons.d` 目录本身永远保留；目录内的未知文件（例如说明文件、备份文件、其他后缀）一律保留。

### KEEP 白名单

以下名称和类别永不进入删除列表：

| 保留项 | 原因 |
| --- | --- |
| `identity.secret`、`identity.public` | 节点身份；重置不改变节点 ID |
| `authtoken.secret`、`metricstoken.secret` | 本地 API / 指标访问凭据 |
| `zerotier-one.port` | 本地管理 API 端口 |
| `local.conf` | 只移除 `settings.tcpFallbackRelay`，文件本身保留 |
| `networks.d`、`peers.d`、`moons.d` | 状态目录本身 |
| `controller.d/`、`trustedpaths` 及其他未列出的名称 | 控制器数据、用户配置或未知文件；采用白名单删除，默认保留 |
| 状态目录中的未知文件 | 例如 `notes.txt`、`*.bak`；不按通配符删除 |

### planet 规则（已核对源码）

ZeroTier 的官方 planet/root 信任数据**内嵌在二进制中**，不是从 peer 列表挑选的，因此本功能不实现“保留前四个 peer”或按节点 ID 猜测的逻辑。

`node/Topology.cpp` 启动时从状态对象 `ZT_STATE_OBJECT_PLANET`（即 `<数据目录>/planet`）读取持久化的 planet，再加载内嵌的官方 planet；两者按 world 时间戳/更新规则比较，较新的一个生效。因此：

- `<数据目录>/planet` 是唯一会被读取的 planet 覆盖文件；可删除的就是根目录下名为 `planet` 的普通文件。
- 删除后，下次启动使用内嵌官方 planet；ZeroTier 可能在启动后重新缓存 `<数据目录>/planet`（内容为当时生效的官方 planet），这是正常行为。
- 大小写不同（如 `Planet`）、`planet.bak`、名为 `planet` 的目录或链接都不会被当作覆盖文件删除。

## local.conf 处理

重置**只移除** `settings.tcpFallbackRelay`。该键在 Moon/中转被清除后已过期；其它字段全部保留，包括：

- `settings.allowTcpFallbackRelay`、`settings.forceTcpRelay`：通用 TCP 回退开关，不是 Moon 派生值，保留；
- `settings.primaryPort`、`secondaryPort`、`tertiaryPort`、`bind`、`physical`、`virtual`、`peerSpecificBonds` 等已知字段；
- 未知顶层字段、未知 `settings` 字段及其值类型。

写入流程复用现有 `LocalConfig` 机制：

1. 通过现有读取器读取并校验当前 `local.conf`（含大小上限、BOM、JSON/根对象/`settings` 对象校验）。
2. 只删除目标键，其它内容原样保留（JSON 重新序列化，键顺序/缩进可能变化，值不变）。
3. 计算原文件 SHA-256；需要提权时把合并结果写入临时文件，提权脚本再次读取当前文件并核对哈希，只有未被外部修改才原子替换；哈希不符或目标变成链接/非普通文件时拒绝写入。
4. 文件不存在：不创建、不改动。
5. 文件为空或只有空白：ZeroTier 按默认值处理，重置视为无变更。
6. 非法 JSON、根值不是对象、`settings` 不是对象，或现有读取器判定的字段类型错误：在停止服务**之前**报错并终止，不删除任何文件、不重启服务。

## 操作顺序

主机通过 `ResetController` 执行一次提权脚本，阶段标记由脚本输出：

1. 主机确认对话框（现有确认机制）→ 取消则零变更。
2. `busy = true`，发出 `interruptionBegan`；宿主调用 `beginServiceInterruption()`、`setBusy(true)`，暂停/作废轮询与业务查询。
3. 主机在真实系统数据目录上建立快照、读取并合并 `local.conf`，生成有序删除列表；任何边界/JSON 错误在此终止（零变更）。
4. 通过现有 `SystemOperations::runElevatedScript()` 提权（Windows UAC / macOS `osascript`）。提权被取消 → 零变更。
5. 脚本校验数据目录、状态目录、所有删除候选的边界与白名单。
6. 检查并停止 `ZeroTierOneService` / launchd 服务，确认状态确实为 Stopped；失败则**不进入删除代码**，并尝试恢复服务。
7. 按白名单删除；任何一项失败即进入失败处理，保留剩余项并报告阶段/明细。
8. 若有变更，用现有原子安装命令替换 `local.conf`（再次核对哈希）。
9. 启动服务，等待 Running，并用现有本地 API 就绪探测（读取 `zerotier-one.port`、`authtoken.secret`，请求本机 `/status`）。
10. 发出 `interruptionEnded`；宿主调用 `endServiceInterruption()`、`setBusy(false)`、`refresh()`，刷新网络/Moon/peer/local.conf 及相关插件状态。发出 `notificationRequested` 反馈结果。

## 失败阶段与处理

| 阶段 | 失败时的行为 |
| --- | --- |
| 计划/读取/JSON（提权前） | 报具体原因，零变更；不会停止服务 |
| `not-installed` | 未检测到服务，零变更 |
| 提权取消 / 无任何标记 | “授权被取消，未做任何更改” |
| `stop` | 未删除任何文件；脚本尝试重新启动服务并报告恢复结果 |
| `cleanup` | 停止后清理失败，可能已部分删除；报告失败项明细并尝试启动服务 |
| `localconf` | 已清理文件但配置写入失败；报告并尝试启动服务 |
| `start` | 文件已清理，但服务启动或本地 API 就绪验证失败；报告并尝试恢复/启动服务 |
| `recovery` | 额外报告“恢复服务失败，请手动启动 ZeroTier 服务” |

日志只记录阶段、白名单文件名/数量和错误文本；不读取、不输出 `identity.secret` / `authtoken.secret` 内容。

## 安全边界

- 数据目录来自 `systemHomeDirectory()`；不跟随 macOS 临时会话目录。
- 主机侧解析出的每个删除候选必须是相对路径且匹配白名单；提权脚本再次校验，双重防护。
- 目标与所有父目录都检查 reparse point（符号链接/junction）；不跟随链接。
- 不递归删除、不对整个数据目录做 glob、不删除状态目录本身。
- 服务未确认停止前绝不删除；停止失败时只尝试恢复服务。
- 有序删除列表写入临时文件并由提权脚本逐行校验；不把可能包含大量 peer 的列表拼进命令行，避免 Windows 命令行长度限制。
- 提权取消、确认取消、提权前校验失败均为零变更。
- 宿主忙时忽略重复请求；`busy`/`canReset` 与宿主的确认/忙门控共同阻止第二次执行。

## QML 与控制器契约

控制器位于 `vusiif::zto::reset`，只依赖现有 `SystemOperations`：

```cpp
class ResetController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool canReset READ canReset NOTIFY canResetChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY statusChanged)
public:
    explicit ResetController(ui::SystemOperations &operations, QObject *parent = nullptr);
    void setBlockedProvider(std::function<bool()> blocked);
    Q_INVOKABLE void requestReset();
    Q_INVOKABLE void confirm(bool accepted);
    bool busy() const;
signals:
    void notificationRequested(const QString &message, bool error);
    void confirmationRequested(const QString &title, const QString &message, const QString &acceptLabel);
    void interruptionBegan();
    void interruptionEnded();
    void busyChanged();
    void canResetChanged();
    void statusChanged();
};
```

宿主负责把 `confirmationRequested` 接到现有确认对话框，把 `accepted` 回传 `confirm()`；`notificationRequested` 接到现有通知条；`interruptionBegan/Ended` 分别接 `beginServiceInterruption() + setBusy(true)` 和 `endServiceInterruption() + setBusy(false) + refresh()`。

`ResetSection.qml` 只使用注入的 `reset` 控制器、`ActionButton`、`AppToolTip` 和 `Theme`，不直接访问后端。确认文案明确包含：退出所有网络、清除本地状态、暂时网络中断、只重启 ZeroTier 服务不重启 Z2。

## 平台与未验证项

- Windows x64 已构建并运行隔离测试，包括在临时目录执行生成的 PowerShell 清理/`local.conf` 写入脚本；测试不会触碰开发机真实 ZeroTier 数据或服务。
- macOS 生成 `launchctl` + `sh` 版本脚本并复用现有会话/服务命令，但真实安装、`osascript` 授权、系统目录 ACL 与真实服务重启未在本机验证。macOS 系统目录可能只有 root 可读；若主机侧无法读取目录或 `local.conf`，操作会在停止服务前报错并保持零变更。需要真机确认。
- Linux 没有既有提权入口，不提供自动重置；脚本生成返回空并由控制器报告“当前平台不支持自动提权”，不执行变更。
