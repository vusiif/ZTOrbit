# 插件源码

后续插件统一放在此目录，每个插件使用独立子目录，例如 `src/plugins/myplugin/`，包含自己的 `CMakeLists.txt`、头文件和实现文件。

插件通过 `ZTGUI::Api` 引用 `src/api/` 提供的 SDK，不依赖 `src/main/` 的内部头文件。`src/main/plugins/` 是宿主的插件管理实现，与此处的插件源码分开维护。

创建、编写和独立编译插件的流程见 [插件开发 SKILL](../../.agent/skills/ztgui-plugin-dev/SKILL.md)，接口说明见 [插件 API 文档](../../docs/plugin-api.md)。

目前 API 支持进程内注册；外部 DLL 的入口协议和自动加载器尚未实现。独立编译成功后，是否需要接入主程序按具体任务决定。
