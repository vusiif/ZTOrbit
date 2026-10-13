---
name: ztgui-build
description: 为 ZTGUI 编译和压缩主程序，按 API 变化更新 SDK 压缩包；仅在用户指定时编译插件。用于本项目的编译、打包和发行产物更新。
---

# ZTGUI 编译与打包

## 默认范围

1. 编译主程序并部署运行依赖，压缩主程序文件夹。
2. API 有变化时独立配置、安装 SDK 并压缩；没有变化则复用已有 SDK 和压缩包。
3. 不编译插件，除非用户明确要求。修改了插件源码、API 或发现现有 DLL 过期，都不自动扩大构建范围。

先定位当前 ZTGUI 工作树根目录，读取根 `AGENTS.md` 和 `VERSION`。不要从全局技能安装路径推断仓库位置。版本由开发者维护，编译与打包不能自行更改 `VERSION`；API 代际取版本号前两段，应用补丁变化不触发 API 更新。

## Windows 入口

当前已验证的交付目标是 Windows x64、MSVC 2022、动态 Qt 6.8+。优先使用用户提供的 Qt SDK；未提供时从现有 CMakeCache 或本机 SDK 定位，不写死某一开发者的路径。

在仓库根目录执行本技能的脚本：

```powershell
$qtSdk = 'C:/Qt/6.8.3/msvc2022_64' # 替换为实际动态 SDK
& ./.agent/skills/ztgui-build/scripts/build-windows.ps1 -QtRoot $qtSdk
```

[build-windows.ps1](scripts/build-windows.ps1) 控制主程序、条件 API 更新与可选插件；[PackageTools.ps1](scripts/PackageTools.ps1) 提供路径检查、内容指纹、干净目录替换和 ZIP 创建。仓库 `scripts/build-windows.ps1` 仅为兼容入口，遵守相同的默认范围。

Windows默认运行包只保留宿主使用的Basic控件样式、中文／英文Qt翻译和运行插件，不带QML调试工具；保留语音需要的Multimedia／FFmpeg。Qt Quick默认使用D3D11，另有无需OpenGL库的CPU绘制模式，因此不默认部署Mesa的 `opengl32sw.dll`。需要显式OpenGL软件兼容环境时添加 `-IncludeOpenGLSoftware`；第三方插件明确导入其他控件样式时添加 `-KeepAllQmlStyles`。裁剪仅发生在已校验的干净暂存目录，不能处理用户已有的插件／数据目录。

脚本用显式目标 `zerotier_gui` 构建主程序。不要用默认“构建全部目标”替代，以免现有构建缓存启用了测试或插件。GUI 回归、服务端、语音面板和音频测试不是普通打包的默认构建目标；根据用户要求或实际代码变化选择需要的验证。

## API 是否更新

API 是头文件 SDK，生成过程是独立 CMake 配置、安装公开头文件/版本头/CMake 导出，再压缩，无须构造额外 API DLL。

脚本将 `src/client/api/` 的文件名及内容哈希、API 代际、安装目录内容和 ZIP 哈希记录在 `cmake-build-api/package-state.json`，对比的是上次成功的 API 打包，不是仅看 Git 是否有未提交改动。

- 源码或 API 代际改变、SDK 缺失/内容异常、首次没有可靠记录时，生成一次干净 SDK 并更新记录。
- SDK 正常但 ZIP 缺失或损坏时，仅重新压缩 SDK。
- API 与产物均未变时，不独立配置/安装 API，不改写其 ZIP；主程序自身的 CMake 配置仍会生成其需要的 API 版本头。
- 用户明确要求重新生成 API 时，加 `-RebuildApi`。

状态记录留在构建缓存中，不进入 SDK 压缩包。API 更新时使用干净安装目录，避免已删除头文件残留。

客户端指纹同时覆盖 `src/shared/include/`，因为 SDK 安装这些公开工具与路由协议。

## Linux 服务端

执行本技能的 `scripts/build-linux.sh`（或根 `scripts/build-linux.sh` 兼容入口）。默认只编译 `zt-server`，生成 `dist/linux-x64/` 与 `dist/linux-x64.tar.gz`，按源码、共享公开头文件、版本与产物指纹条件更新 `dist/ZTServer-api-v<API版本>/` 与 ZIP。脚本不安装/卸载真实 ZeroTier，不停系统服务。

用户明确指定语音服务端插件时添加 `--build-voice-plugin`，独立发布到 `dist/server-plugins/ztserver_voice_chat.so`，不带入宿主运行包。目标 Linux 需匹配的 Qt6 Core/Network 动态运行库。SDK 消费者、通用路由、模拟安装脚本及语音测试位于 `src/tests/server/` 与 `src/tests/voice-chat/`；测试与运行数据不进入发行包。

## 用户指定插件时

仅构建用户要求的插件，并先读项目 `.agent/skills/ztgui-plugin-dev/SKILL.md`。明确指定语音插件时，使用：

```powershell
& ./.agent/skills/ztgui-build/scripts/build-windows.ps1 -QtRoot $qtSdk -BuildVoicePlugin
```

其他插件按其独立项目和公开 SDK 构建，不把所有插件一起编译。插件文件平铺到 `dist/plugins/`，如 `ztgui_voice_chat.dll`，许可证也直接位于该目录，不额外嵌套文件夹。

默认流程可以读取已有 DLL 的依赖信息，为主程序部署匹配的 Qt 运行库，但不能因此重编或改写插件产物。API 代际或工具链变化可能使旧 DLL 失效，应报告需要重编的插件，由用户决定构建范围。

## 产物与完成条件

- 主程序：`dist/windows-x64/` 和 `dist/windows-x64.zip`。
- SDK：`dist/ZTGUI-api-v<API代际>/` 及同名 `.zip`。
- 指定插件：`dist/plugins/<插件文件>`，主程序压缩包不带应用插件。

运行依赖与主程序一起部署；已有插件的可选依赖可复用其 DLL 提供的静态依赖信息。缺少插件产物时只部署主程序本身的依赖，不能为补齐依赖而自动编译插件。

使用干净暂存目录，完成后替换发行目录，保留原目录备份；不把日志、插件数据、模拟 CLI 或测试可执行文件带入包。每步确认成功再使用产物，失败不能宣称新包已完成。

核对输出目录与压缩包的文件数量/内容校验及版本信息。API 变化时按需要验证安装后 SDK 的独立引用；指定插件时验证实际加载。交付说明主程序 ZIP、API 更新或复用的原因、是否构建了指定插件。

macOS 仅在用户指定时按仓库 `docs/macos.md` 调整工具链，仍遵守上面三项范围。现有 `scripts/build-macos.sh` 包含全量插件构建，未指定插件时不能直接作为默认入口；未实机运行不能声称已验证。
