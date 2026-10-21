# 语音服务端插件（Java）

`ztserver_voice_chat.jar` 是 ZTServer 的 Java 插件，路由 ID `voice-chat`、显示名“语音聊天”、版本 0.1.0、API 0.2。成员房间与 `ZTV1` 音频协议与旧 C++ 插件完全一致；客户端无需修改。

## 构建与部署

需要 JDK 25 与 Maven（宿主 API 由根项目安装到本地 Maven 仓库）：

```bash
revision="$(tr -d '\r\n' < SERVER_VERSION)"
api_version="$(printf '%s' "$revision" | sed -E 's/^0\.([0-9]+)\.[0-9]+(-[0-9A-Za-z.-]+)?$/0.\1\2/')"
mvn -f src/server/pom.xml -Drevision="$revision" -DapiVersion="$api_version" -pl api -am -DskipTests install
mvn -f src/plugins/voice-chat/server/pom.xml -DapiVersion="$api_version" clean package
# 产物：src/plugins/voice-chat/server/target/ztserver_voice_chat.jar
```

或使用发行脚本一次性构建：`bash scripts/build-linux.sh --build-voice-plugin`，结果在 `build/dist/server-plugins/ztserver_voice_chat-v0.1.0.jar`，不进入宿主包。

把 JAR 放入宿主工作目录的 `plugins/` 后重启：

```bash
cd /root/Server/ZTServer
install -m644 /path/ztserver_voice_chat.jar plugins/
systemctl restart zt-server.service
```

插件加载要求 manifest `apiVersion` 与宿主完全一致；构建脚本使用根 `SERVER_VERSION` 的前两段数字和同一预发布后缀（当前 0.2）。

## 运行规则

- 默认 32 个房间、每房间 16 人、256 个语音会话；`plugins-data/语音聊天/settings.json` 可设置 `maxRooms`(1–10000)、`maxMembers`(1–256)、`maxConnections`(1–10000)，重启生效。宿主总连接上限仍为 256。
- 房间开放且临时，一个会话只能加入一个房间；最后成员退出后删除，重启清空。
- 业务控制 JSON 走 `ZTS1` 路由 `voice-chat`，最大 64 KiB；支持 `hello/rooms/create/join/leave/mute/ping` 和 `rooms/members` 事件，错误码与旧实现一致。
- UDP 音频走同一路由的 `ZTV1` 包（30 字节头 + 负载，单包最多 1200 字节）。首次注册要求来源 IP 与 TCP peer 一致并绑定端口；退房/静音/过期 token 不再转发；每个接收者使用自己的 token，房间隔离。
- 每会话控制请求每秒 30 次、UDP 包每秒 100 次；15 秒无控制请求清理会话；控制发送积压超过 256 KiB 时延迟关闭连接。
- 插件在宿主事件线程处理回调；停止时取消定时清理任务并清空会话。

## 测试

隔离模式需要插件 JAR；宿主以临时目录启动并复制 JAR：

```bash
python3 src/tests/voice-chat/ServerTests.py \
  --binary src/server/scripts/start-server.sh \
  --jar src/server/main/target/zt-server.jar \
  --plugin src/plugins/voice-chat/server/target/ztserver_voice_chat.jar
```

也可以直接使用 fat jar：

```bash
python3 src/tests/voice-chat/ServerTests.py \
  --binary src/server/main/target/zt-server.jar \
  --plugin src/plugins/voice-chat/server/target/ztserver_voice_chat.jar
```

已部署服务（不创建隔离服务器，容量/超时测试自动跳过）：

```bash
python3 src/tests/voice-chat/ServerTests.py --host 10.10.10.1 --port 18100
```

6 项隔离测试覆盖成员身份/旧客户端、房间生命周期、UDP 隔离/静音/过期凭据、容量、畸形控制和超时清理。真实多人麦克风/耳机听感须另行验收。
