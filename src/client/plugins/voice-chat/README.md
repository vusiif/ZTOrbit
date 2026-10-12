# 语音聊天插件

`client-plugin/` 独立编译 ZTGUI 动态插件。服务端插件位于 `src/server/plugins/voice-chat/`，由新宿主 `zt-server` 加载；双方共享根 `src/shared/voice-chat/Protocol.h` 与路由协议。

插件文档由本项目自行维护，构建、部署、协议及验证说明见 [docs/voice-chat.md](docs/voice-chat.md)。
