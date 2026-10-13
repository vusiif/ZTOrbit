# 插件源码

后续插件统一放在 `src/plugins/<name>/`，按项目整合 `client/`、`server/` 和 `shared/`。两端各自包含独立的 `CMakeLists.txt`、头文件、实现和 `docs/`；仅有一端的插件只需对应子目录。

客户端插件通过 `ZTGUI::Api` 引用 `src/client/api/` 提供的 SDK，服务端插件通过 `ZTServer::Api` 引用 `src/server/api/`。插件不依赖两端 `main/` 的内部头文件；宿主的插件管理实现与此处的插件源码分开维护。

创建、编写和独立编译插件的流程见 [插件开发 SKILL](../../.agent/skills/ztgui-plugin-dev/SKILL.md)，接口说明见 [客户端 API](../../docs/plugin-api.md) 与 [服务端 API](../../docs/server-api.md)。

动态插件实现公开 `PluginInterface` 并携带 SDK 生成的 API 元数据；ZTGUI 启动时扫描可执行文件旁的 `plugins/` 并注册菜单，增减插件后重启即可。

语音项目维护在 `voice-chat/`，两端共用 `voice-chat/shared/Protocol.h`。构建与部署见 [客户端语音文档](voice-chat/client/docs/voice-chat.md) 与 [服务端语音文档](voice-chat/server/docs/voice-chat.md)。
