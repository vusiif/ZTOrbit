# 本机 ZeroTier TCP 回退配置

设置页的“本机 ZeroTier TCP 回退”直接管理 ZeroTier 的系统级 `local.conf`。该文件是这些设置的唯一来源；ZTOrbit 不把开关或中转地址另存到 `settings.ini`。部署拓扑、端口和 Moon 地址约定统一见[网络部署与默认连接约定](network-conventions.md)。

## 配置文件位置

| 平台 | 路径 |
| --- | --- |
| Windows | `C:\ProgramData\ZeroTier\One\local.conf` |
| macOS | `/Library/Application Support/ZeroTier/One/local.conf` |
| Linux | `/var/lib/zerotier-one/local.conf` |

路径由现有 `dataDirectory()`／`systemHomeDirectory()` 解析。macOS 使用真实系统目录，不写入只包含本地 API 凭据和 Moon 文件的会话缓存。

启动时读取实际文件；文件不存在且 ZeroTier 配置目录存在时创建：

```json
{
    "settings": {
        "allowTcpFallbackRelay": true,
        "forceTcpRelay": false
    }
}
```

不会自动写入 `tcpFallbackRelay`。已有文件不会在启动时被默认值覆盖；缺少开关字段时界面分别按 `true`、`false` 显示，但不因此改写文件。文件不是合法 JSON、根值不是对象或 `settings` 不是对象时显示错误并保留原文件。ZeroTier 未安装、目录不可用或授权被取消时只显示原因，不阻塞主程序，可重试。

## 写入规则

每次保存都重新读取当前文件、校验、只合并目标字段，再原子替换。`primaryPort`、其他已知和未知字段均保留。普通保存使用 `QSaveFile`；需要系统权限时复用现有提权入口，由提权脚本在替换前再次校验当前文件哈希，文件已被外部修改时拒绝覆盖。保存失败后重新读取实际文件，界面不会保留未落盘的值。

开关规则：

- “允许TCP回退”默认开启，对应 `settings.allowTcpFallbackRelay`。
- “强制TCP回退”默认关闭，对应 `settings.forceTcpRelay`。
- 开启强制时允许也开启；关闭允许时强制同时关闭。两个字段一次写入，不产生 `allow=false/force=true` 的中间状态。
- 初始化、页面重新读取和绑定更新不会触发保存。

这些是本机 ZeroTier 节点级设置，影响本机加入的所有网络。保存后不自动重启 ZeroTier。ZeroTier One 1.16.2 约每 10 秒检查并热加载 `local.conf`，但修改中继地址不会主动断开已有 TCP tunnel，因此切换中转地址后仍需手动重启以刷新现存连接。

## 选择 TCP 回退中转服务器

“切换TCP回退中转服务器”按需异步读取 `listnetworks` 和 `listmoons`，不依赖用户先打开 Moon 页面。候选网络与 `.z2bundle` 导出使用同一资格判断（本机存在按网络 ID 关联的 Moon 文件）。

服务器公网 IPv4 只从对应 Moon 的服务器根节点 `stableEndpoints` 获取，并按[网络约定](network-conventions.md)的 ID 关系校验：

- 网络 ID 前 10 位 = 服务器 ZTID；
- Moon ID = `000000` + 服务器 ZTID；
- 根节点 `identity` 的地址前缀 = 服务器 ZTID。

不使用虚拟网段 `.1`、客户端 IP、任意 peer 地址，也不解析二进制 `.moon` 文件。多个合规 IPv4 时按 `stableEndpoints` 顺序取第一个，并在候选项中注明。中继端口固定 `9443/TCP`；界面和 `.z2bundle` 显示 `IP:9443`，写入 `local.conf` 的原生格式是 `IP/9443`。无法可靠确定 IP 的网络项禁用并显示原因。

选择不同端点并成功保存后弹窗提醒手动重启 ZeroTier；取消、重复选择相同端点或保存失败都不提示切换成功。该操作只改 `settings.tcpFallbackRelay`，不改变两个开关，不提供自动重启。

## 配置包字段

`.z2bundle` 的 `version=1` 格式新增可选顶层字段：

```json
"tcpFallbackRelay": "1.2.3.4:9443"
```

导出时重新读取 `local.conf`，仅当解析后的当前 IP 与导出网络对应 Moon 服务器公网 IPv4 相同且端口为 9443 时携带该字段；否则省略，原有网络和 Moon 内容正常导出。不导出整份 `local.conf`、身份、令牌或开关状态。

导入时先完整校验包及该字段，再执行任何系统写入。字段缺失时保留本地值；字段存在且有效时只覆写 `settings.tcpFallbackRelay`，保留两个开关和其他字段。`local.conf` 不存在时先按初始内容创建再合并。非法类型、空值、非法 IPv4 或端口不是 9443 时明确报错。Moon 替换确认被取消时不会提前修改 TCP 配置；配置写入与 Moon 复制、服务重启合并在同一次提权操作中，配置在该次重启前写入，不为新字段额外重启。

## 平台边界

Windows 通过现有提权入口在需要时执行 PowerShell 写入；macOS 通过现有 `osascript` 提权脚本读写系统目录；Linux 不新增提权入口，目录不可写时显示错误，需以具备相应权限的方式运行后重试。未放宽系统目录权限，也未新增自动重启入口。模拟目录和脚本测试不会读写开发机的真实 `local.conf`，也不会重启真实 ZeroTier 服务。
