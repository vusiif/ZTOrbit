# 项目命名

代码和项目名称保持 **ZTOrbit**（`zto`）及 **ZTOrbitHub**（`ztohub`）；对外交付的客户端产物使用 **Z2**，服务端产物使用 **Z2Hub**。

客户端主窗口及启动错误窗口的标题为 **Z2**。

远程仓库为 [vusiif/ZTOrbit](https://gitee.com/vusiif/ZTOrbit)，Git `origin` 地址为 `https://gitee.com/vusiif/ZTOrbit.git`。

| 用途 | 客户端 | 服务端 |
| --- | --- | --- |
| 内部构建目标 | `zto`、`zto_voice_chat`、`ztplugin_pack` | Maven artifactId `ztohub`、`ztohub-api` |
| 程序文件 | `Z2.exe`；macOS `Z2.app` | `Z2Hub.jar`、`z2hub.service`、`z2-installer.sh` |
| 发行目录和压缩包 | `Z2-windows-x64/`、`Z2-windows-x64.zip` | `Z2Hub-linux-x64/`、`Z2Hub-linux-x64.tar.gz` |
| 插件文件 | `z2_<模块>-v<版本>.z2plugin`；worker `z2_media_worker.exe` | `z2hub_<模块>-v<版本>.jar` |
| 配置导出 | `.z2bundle` | — |
| 命名空间／Java 包 | `vusiif::zto` | `vusiif.ztohub`；归档 C++ 为 `vusiif::ztohub` |
| 宏和构建变量前缀 | `ZTO_` | `ZTOHUB_` |
| CMake SDK 包 | `ZTOrbitPluginApi`、`ZTOrbitMediaApi`、`ZTOrbitMediaEngine` | 归档 C++：`ZTOrbitHubPluginApi` |
| SDK 导出目标 | `ZTO::Api`、`ZTO::Services`、`ZTO::Protocol`、`ZTO::MediaApi` | 归档 C++：`ZTOrbitHub::Api` |
| SDK 目录 | `Z2-api-v0.5` | `Z2Hub-api-v0.2` |
| 媒体 SDK 目录 | `Z2-media-api-v2`、`Z2-media-engine-sdk-v1` | — |
| Java Maven 坐标 | — | `vusiif.ztohub:ztohub-api:0.2` |
| 插件入口 | `vusiif.zto.api.PluginInterface/3.0` | `META-INF/ztohub-plugin.json` |

macOS 应用包为 `Z2.app`，QML 模块为 `ZTOrbit`。项目技能为 `.agents/skills/zto-build/` 和 `.agents/skills/zto-plugin-dev/`。

SDK 封装工具文件名为 `z2plugin_pack`（Windows 加 `.exe`），CMake 构建目标仍为 `ztplugin_pack`。服务端 SDK 内的 Maven 仓库路径和 `ztohub-api-<版本>.jar` 保持坐标要求的命名。命名空间、头文件路径、宏、IID、插件 ID、包内格式 magic、网络协议及 `.zto` 数据目录保持不变。

客户端导入、扫描、删除和文件选择器统一使用 `.z2plugin`，不添加旧扩展名别名。已有插件包仅改扩展名即可继续使用，包格式和 API 兼容规则不变；旧配置包可改为 `.z2bundle` 后导入。发行构建会生成新文件名和包内 worker 入口，部署时应更新路径及 systemd unit。

## 版本与迁移

2026-10-31 按开发者指定，客户端为 `0.5.2`，服务端为 `0.2.2`；两端 API 分别为 `0.5` 和 `0.2`。版本仍从根 `VERSION`／`SERVER_VERSION` 派生，客户端移除日期后缀。插件自身版本保持不变。

旧 `ztgui`／`ZTGUI`、`ztserver`／`ZTServer` 和 `zt-server` 的当前接口、构建入口、头文件目录、Java 包、IID、QML 资源路径及 SDK 包统一替换。不提供旧名称别名；全部受影响插件须使用新 SDK 重编。网络导入包的格式标识由 `zerotier-gui/network-moon` 改为 `zto/network-moon`；旧格式包须重新导出。现有部署请更新服务文件、安装目录及启动命令。

旧设置注册表／INI 的白名单导入继续使用历史来源 `ZeroTierGUI/ZTGUI` 和 `ZeroTierGUI/ZeroTier GUI`。这些字符串仅在迁移实现及对应测试中保留，不用于新程序身份或接口。程序自身设置和缓存使用新的品牌及 `.zto` 元数据目录。

`CHANGELOG.md` 中的历史条目保留当时的程序名和版本。当前接口和构建入口以本文为准。Git remote 和工作树目录属于实际外部地址，不随代码内更名自动改变。
