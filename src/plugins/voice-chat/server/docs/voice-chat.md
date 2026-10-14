# 语音服务端插件

`ztserver_voice_chat.so` 是新宿主 `zt-server` 的独立插件，路由 ID 为 `voice-chat`，名称“语音聊天”。只依赖公开 `ZTServer::Api` 和 `src/plugins/voice-chat/shared/Protocol.h`，没有 Widgets、Multimedia、Opus 或宿主内部头文件依赖。与客户端共用业务协议，使用宿主的 TCP/UDP 收发工具。

## 构建与配置

```bash
cmake -S src/server -B cmake-build-server -DCMAKE_BUILD_TYPE=Release -DZTSERVER_BUILD_VOICE_PLUGIN=ON
cmake --build cmake-build-server --target zt-server ztserver_voice_chat --parallel
```

独立编译插件时使用 `cmake -S src/plugins/voice-chat/server -B cmake-build-server-voice -DCMAKE_PREFIX_PATH=/path/to/ZTServer-api-v0.4`，并保留同项目的 `shared/`；共享协议路径可通过 `-Dvoice_protocol=/path/to/shared` 指定，该目录直接包含 `Protocol.h`。产物位于构建目录的 `plugins/`，插件自身版本0.1.0，与宿主API0.4精确匹配。目录迁移后使用新的构建目录或重新生成旧 CMake 缓存。

将 `.so` 放到启动目录 `plugins/`，宿主默认使用同一个端口 TCP/UDP18100。插件默认32个房间、每房间16人、256个语音会话；可在 `plugins-data/语音聊天/settings.json` 设置 `maxRooms/maxMembers/maxConnections`，重启生效。宿主总连接上限仍为256。

房间开放且临时，一个会话只能加入一个房间，最后成员退出后删除，重启清空。业务控制 JSON、房间凭据和 `ZTV1` 音频包均位于统一 `ZTS1` 包的业务负载内。TCP 控制 JSON 最大64 KiB，保留 hello/rooms/create/join/leave/mute/ping，以及 rooms/members 事件；UDP 内层最大1200字节。详细业务字段与音频规则见 [客户端语音文档](../../client/docs/voice-chat.md)。

UDP 首次注册要求来源 IP 与 TCP 一致，成功后绑定端口；音频只接受有效房间凭据和绑定端点。退房凭据作废，只向同房间其他成员转发，每个接收者使用自己的凭据。每会话控制请求每秒30次、UDP包100次；15秒无控制请求清理会话，控制发送积压超过256KiB时排队关闭连接。初始化、房间进出和停机使用双写日志，日志不包含音频负载或凭据。

## 部署与回滚

当前部署目录为 `/root/Server/ZTServer`，systemd unit 为 `zt-server.service`，以root运行、监听 `0.0.0.0:18100`。首次迁移先停止并禁用旧 `ztgui-voice.service`，保留 `/opt/ztgui-voice` 和旧unit；新宿主启动成功后用适配过的客户端插件连接。旧客户端和新宿主的外层协议不兼容，必须同时更新。

回滚时停止并禁用 `zt-server.service`，再 `systemctl enable --now ztgui-voice.service`；客户端恢复旧语音DLL。现有ZeroTier、Moon、ztncui与其他服务不参与替换。

隔离测试：`python3 src/tests/voice-chat/ServerTests.py --binary /absolute/path/to/zt-server`；已部署服务测试：`python3 src/tests/voice-chat/ServerTests.py --host 10.10.10.1 --port 18100`。已部署测试仅创建临时房间，不修改容量或真实网络配置。真实多人麦克风/耳机听感须另行验收。

## 音量与新封装

客户端输入、总输出及其他成员的本机播放音量支持 0–200%；输入和成员默认100%，总输出迁移既有80%设置。输入在编码前增益，成员在混音前增益，总输出在混音后增益并最后限幅；静音优先，不修改系统音量。输入和总输出持久化，成员音量只对本机、当前房间会话有效，退房清空，切换设备保留。

两端技术 ID 为 `voice-chat`，显示名和数据目录仍为“语音聊天”。协议业务版本仍为1，路由 ID 同步改变；客户端入口／生命周期 IID 为 /3.0，服务端 /2.0，两端旧动态库需重编。SDK CMake 辅助函数构建后生成 `ztgui_voice_chat.ztplugin` 或 `ztserver_voice_chat.ztplugin`，正式宿主只加载该封装包；没有接入多中转音频传输。

2026-10-14：语音客户端及Linux服务端已重新编译为API0.3的加密 .ztplugin，分别发布到 dist/plugins/ztgui_voice_chat.ztplugin 和 dist/server-plugins/ztserver_voice_chat.ztplugin。Windows使用安装后的客户端SDK、Qt6.8.3/MSVC2022/x64 Release；真实封装包的动态加载、页面创建、音频设置、连接／断开、异步卸载和再次加载通过，音频／页面源码测试两组通过。Linux Qt6.4.2服务端插件的5项隔离语音集成全部通过，涵盖容量、超时、UDP隔离、静音和过期凭据。未连接已部署服务器，未进行真实多人麦克风／耳机听感验收。

## v2 独立 SDK 构建

Linux 服务端使用安装后的 ZTServerPluginApi 0.4 SDK 独立配置，显式构建 `ztserver_voice_chat_package` 得到 v2 包。正式 install 仅安装 `.ztplugin` 和声明的许可证；业务 SO 为中间产物。无录放音需求，不部署 Multimedia/FFmpeg，不捆绑系统 Qt。SDK 工具和容器兼容规则见[运行依赖分离](../../../../../docs/plugin-runtime-separation.md)。

2026-10-14 运行依赖分离验收：v2 正式产物、独立 SDK 构建、实际加载结果和明确未验证的平台/听感项目统一记录在[本轮验收](../../../../../docs/plugin-runtime-separation.md#验证记录)，其中客户端完成真实设备零增益回送及20轮热卸载。

2026-10-14 版本升级：宿主0.4.1、SDK0.4，当前插件须使用API0.4重编；客户端/3.0和服务端/2.0 IID保持不变，上方API0.3记录为历史验证。
