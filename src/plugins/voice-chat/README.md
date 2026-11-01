# 语音聊天插件

本项目整合客户端、服务端和共享业务协议：

- `client/`：独立编译的 ZTOrbit 动态插件。
- `server/`：由 `ztohub` 加载的独立服务端插件。
- `shared/`：客户端与归档 C++ 服务端共用的 `Protocol.h`；Java 服务端在 `VoiceProtocol.java` 中按字节兼容重新实现。

宿主的公开工具和通用路由协议仍通过各自 SDK 提供。

插件文档由本项目自行维护，构建、部署、协议及验证说明见 [客户端文档](client/docs/voice-chat.md) 与 [服务端文档](server/docs/voice-chat.md)。
