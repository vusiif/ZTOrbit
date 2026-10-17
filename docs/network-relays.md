# 多中转应用数据通信

本机制控制 ZTGUI 应用数据的转发节点，不指定 ZeroTier 内核的物理中转路径。默认网络为 IPv4 /24，网络 ID 前十位对应控制端节点 ID 和默认 Moon，控制端在虚拟 IP `.1` 运行 ZTServer（TCP/UDP 18100）。所有参与节点必须在该网络已授权。

Java 宿主实现位于 `src/server/main/src/main/java/vusiif/ztgui/server/main/network/`（`NetworkProtocol`、`NetworkTransport` 等），行为与旧 `src/shared/network/NetworkTransport.cpp` 保持一致；客户端实现位于 `src/client/main/ClientNetworkService.cpp`，本次 Java 服务端迁移没有改动客户端协议，工作树中的按需启停改动属于客户端优化，不改变线协议。

## 控制端登记

ZTServer 工作目录的 `relays.json` 按网络登记额外节点，启动读取；默认 `.1` 自动加入。示例：

```json
{
  "0123456789abcdef": [
    {"nodeId":"0000000005", "moonId":"0000000000000005", "virtualIp":"10.10.10.5", "port":18100}
  ]
}
```

额外节点同样加入该网络、获得登记的虚拟 IP 并运行 ZTServer。插件无须安装在中转上；转发是宿主内置能力。发布列表由默认控制端提供，应用验证中转节点授权与虚拟 IP 对应关系。列表修改后重启控制端；不增加远程修改配置操作。

客户端使用本机虚拟 IP UDP 18101，服务端沿用 18100；接收能力每轮登记，控制端发布活动接收端口。两种宿主同节点运行时优先客户端接收器。Windows／Linux 防火墙应允许这些端口在目标 ZeroTier 网络内通信。

## 选择、缓存及失败

传输使用独立的单线程 Netty event loop 独占状态表，宿主事件线程只接收回调快照；每 2 秒刷新并探测，探测超时 1 秒。平均 RTT 是最近 10 次成功样本的算术平均，未知为 -1；连续 3 次失败或 10 秒没有成功响应视为不可用。已授权成员地址仅缓存 30 秒，不会从未知成员 ID 推算 IP。

默认广播固定 `.1`，失效后直接发送。多中转先保留 RTT 不超过最低值加 `max(5ms,最低值×20%)` 的节点，再比较转发字节速率、排队字节及本机预计工作量；分配键为网络／插件／发送成员／目标成员。故障立即重选，连续 3 次超出延迟集合后重选，空闲 30 秒释放绑定。每个中转收到一份负载和其负责的目标子列表。

数据通过 ZTS1/builtin 下的 ZTD1 交付格式传输：网络 ID 8 字节、发送与目标 ID 各 5、消息 ID 16、插件 ID 长度 1 字节及 ASCII ID、二进制数据。总长不超过 1232 字节；`maxNetworkDataBytes(id)` 给出负载上限（实现见 `NetworkProtocol.java`）。中转上传使用既有 TCP 长度帧和 `network.forward` 操作；其他内置操作为 `network.relays`、`network.register` 和 UDP `network.probe`。

宿主验证来源虚拟 IP、发送成员、网络授权和目标映射，拒绝任意地址、无效目标、跨网络发送及继续中转。请求目标最多 4096 个，TCP 总长沿用 1 MiB；消息去重记录最多 8192 条、保留 30 秒。确认丢失后直发沿用同一消息 ID。发送成功只表示 UDP 队列接受，不保证远端收到、顺序或业务成功，不支持分片。

语音保留独立入房与音频协议，Java 语音插件技术路由 ID 为 `voice-chat`，不接入多中转广播。移植后的验证范围见 `docs/validation/ztserver-java-2026-10-17/`。
