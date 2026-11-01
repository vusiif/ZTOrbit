# 插件运行依赖交付

> 2026-10-17：Linux 服务端已迁移为 Java 25 + Netty，不再加载 C++ `.z2plugin`，也不需要宿主侧 Qt 运行库；本文中“服务端入口 `/2.0`”和“服务端 Core/Network 契约”仅适用于 `src/server-old/` 的归档实现。Java 插件格式、SDK 与线程约定见[服务端 API](server-api.md)。

当前客户端 `0.5.2`、客户端 API `0.5`，服务端 `0.2.2`、服务端 API `0.2`；客户端入口／生命周期 IID `/3.0`，旧 C++ 服务端入口 `/2.0`。媒体现使用独立 v3 worker 包；语音 v3 只依赖媒体公开 API。通用契约见[插件服务](plugin-services.md)，业务边界见[媒体](../src/plugins/media/client/docs/media.md)和[语音文档](../src/plugins/voice-chat/client/docs/voice-chat.md)。工作进程目前只验证 Windows x64；旧 C++ 服务端归档在 `src/server-old/`。

## 收集与安装 SDK

`zto_package_plugin()` 保留普通插件 v2 默认行为。增加 `SERVICE_MANIFEST` 时输出 v3；同时支持 `RUNTIME_QT_MODULES`、`RUNTIME_FILES`、`LICENSE_FILES`，映射使用 `包内路径=源路径`。POST_BUILD 和显式 `<target>_package` 共用文件锁，重新收集实际闭包并原子发布；正式安装只有 `.z2plugin` 和许可证，裸 DLL／EXE 留在构建目录。

Windows 收集器在校验过的独立暂存目录运行实际 Kit 的 windeployqt，不读取宿主发布目录或其他业务插件缓存。进程内入口分析 DLL，排除宿主基础 Qt、系统库和 CRT；工作进程分析 EXE，携带自己的基础 Qt、声明模块／后端、实际可达第三方库和 MSVC 运行库。worker 不继承进程内“排除基础 Qt”的规则；其 requiresHost 必须为空，运行环境只在子进程生效。旧进程内媒体包被拒绝。

HostRuntimeContract.cmake 保留基础 ABI 契约：客户端 Core、Gui、Network、Qml、Quick、QuickControls2、Widgets、Concurrent；服务端 Core、Network。进程内插件匹配 Qt 主次版本、编译器、架构及构建模式。worker 的 Qt 构建 ID／版本记录自己的 Kit，宿主按协议和能力握手，不要求同一个 Qt Kit。

客户端 SDK 的 `sdk-tools.json` 记录格式支持、安装文件与源码输入的 SHA-256；Java 服务端 SDK 使用 `sdk.json` 记录 API／程序版本、JDK 要求与输入指纹。客户端 SDK 指纹覆盖公开头、共享包实现及全部 helper／收集脚本。独立插件构建入口校验 SDK 内容及本地存在的源码输入，过期立即报错；额外公开 SDK 用 `-AdditionalSdk`，额外 CMake 配置用参数数组 `-CMakeArguments`。

## 许可证

收集器使用实际 Kit 的 licenses/，包括 Qt LGPLv3 和媒体许可。FFmpeg 还需要 `licenses/FFmpeg/actual-build.json`，记录实际导出的版本、构建配置、许可和每个分发二进制 SHA-256；哈希必须与实际文件一致，不能替换为其他系统安装的许可。Opus 1.6.1 的 COPYING 随媒体包交付。许可证既在认证包内，也导出为独立可读目录。

## 格式与清理

v2/v3 继续逐块认证、逐文件散列，文件数最多 512，单文件最多 256 MiB，总明文最多 512 MiB；拒绝绝对路径、路径穿越、重复／文件目录冲突、链接输入、截断和尾随数据。v3 的执行与依赖声明包含在认证头中。共享格式密钥是封装机制，不是发行者签名。

旧普通插件可保留非 Qt 运行库到进程退出；该兼容机制不用于媒体服务。v3 worker 的库从未映射到宿主；卸载需要同时证明业务 DLL 已解除映射、拥有的进程树退出、租约与调用排空、提取锁已释放。目录删除失败可有界重试，不能用删除目录或 QPluginLoader::unload 返回值替代模块／进程核对。
