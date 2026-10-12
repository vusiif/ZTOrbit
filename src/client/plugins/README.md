# 插件源码

后续插件统一放在此目录，每个插件使用独立子目录，例如 `src/client/plugins/myplugin/`，包含自己的 `CMakeLists.txt`、头文件和实现文件。

插件通过 `ZTGUI::Api` 引用 `src/client/api/` 提供的 SDK，不依赖 `src/client/main/` 的内部头文件。`src/client/main/plugins/` 是宿主的插件管理实现，与此处的插件源码分开维护。

创建、编写和独立编译插件的流程见 [插件开发 SKILL](../../../.agent/skills/ztgui-plugin-dev/SKILL.md)，接口说明见 [插件 API 文档](../../../docs/plugin-api.md)。

动态插件实现公开 `PluginInterface` 并携带 SDK 生成的 API 元数据；ZTGUI 启动时扫描可执行文件旁的 `plugins/` 并注册菜单，增减插件后重启即可。

语音客户端维护在 `voice-chat/client-plugin/`，服务端插件迁至 `src/server/plugins/voice-chat/`，业务协议共用 `src/shared/voice-chat/Protocol.h`。客户端构建见 [语音文档](voice-chat/docs/voice-chat.md)。
