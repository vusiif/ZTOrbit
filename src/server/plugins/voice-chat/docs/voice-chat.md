# 语音服务端插件

`ztserver_voice_chat.so` 是新宿主 `zt-server` 的独立插件，路由 ID 为 `org.ztgui.voice-chat`，名称“语音聊天”。只依赖公开 `ZTServer::Api` 和 `src/shared/voice-chat/Protocol.h`，没有 Widgets、Multimedia、Opus 或宿主内部头文件依赖。与客户端共用业务协议，使用宿主的 TCP/UDP 收发工具。

## 构建与配置

```bash
cmake -S src/server -B cmake-build-server -DCMAKE_BUILD_TYPE=Release -DZTSERVER_BUILD_VOICE_PLUGIN=ON
cmake --build cmake-build-server --target zt-server ztserver_voice_chat --parallel
```

独立编译插件时使用 `cmake -S src/server/plugins/voice-chat -B cmake-build-server-voice -DCMAKE_PREFIX_PATH=/path/to/ZTServer-api-v0.3`，并保留仓库 `src/shared/`；共享协议路径可通过 `-Dvoice_protocol=/path/to/shared` 指定。产物位于构建目录的 `plugins/`，插件自身版本0.1.0，与宿主API0.3精确匹配。

将 `.so` 放到启动目录 `plugins/`，宿主默认使用同一个端口 TCP/UDP18100。插件默认32个房间、每房间16人、256个语音会话；可在 `plugins-data/语音聊天/settings.json` 设置 `maxRooms/maxMembers/maxConnections`，重启生效。宿主总连接上限仍为256。

房间开放且临时，一个会话只能加入一个房间，最后成员退出后删除，重启清空。业务控制 JSON、房间凭据和 `ZTV1` 音频包均位于统一 `ZTS1` 包的业务负载内。TCP 控制 JSON 最大64 KiB，保留 hello/rooms/create/join/leave/mute/ping，以及 rooms/members 事件；UDP 内层最大1200字节。详细业务字段与音频规则见 [客户端语音文档](../../../../../src/client/plugins/voice-chat/docs/voice-chat.md)。

UDP 首次注册要求来源 IP 与 TCP 一致，成功后绑定端口；音频只接受有效房间凭据和绑定端点。退房凭据作废，只向同房间其他成员转发，每个接收者使用自己的凭据。每会话控制请求每秒30次、UDP包100次；15秒无控制请求清理会话，控制发送积压超过256KiB时排队关闭连接。初始化、房间进出和停机使用双写日志，日志不包含音频负载或凭据。

## 部署与回滚

当前部署目录为 `/root/Server/ZTServer`，systemd unit 为 `zt-server.service`，以root运行、监听 `0.0.0.0:18100`。首次迁移先停止并禁用旧 `ztgui-voice.service`，保留 `/opt/ztgui-voice` 和旧unit；新宿主启动成功后用适配过的客户端插件连接。旧客户端和新宿主的外层协议不兼容，必须同时更新。

回滚时停止并禁用 `zt-server.service`，再 `systemctl enable --now ztgui-voice.service`；客户端恢复旧语音DLL。现有ZeroTier、Moon、ztncui与其他服务不参与替换。

隔离测试：`python3 src/tests/voice-chat/ServerTests.py --binary /absolute/path/to/zt-server`；已部署服务测试：`python3 src/tests/voice-chat/ServerTests.py --host 10.10.10.1 --port 18100`。已部署测试仅创建临时房间，不修改容量或真实网络配置。真实多人麦克风/耳机听感须另行验收。
