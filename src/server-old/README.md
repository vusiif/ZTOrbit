# ZTServer（旧 C++ 实现，已归档）

本目录归档了 2026-10-17 之前使用的 C++17 / Qt6 服务端实现，仅用于历史构建与对照，不再是仓库的正式服务端。正式服务端已迁移到 [`../server/`](../server/README.md)（Java 25 + Netty，Maven 构建）。

## 内容

- `api/`：旧公开插件 SDK（`ZTServerPluginApi`，Qt 动态插件 IID `vusiif.ztgui.server.api.PluginInterface/2.0`）。
- `main/`：旧宿主（`ServerHost.cpp`、`PluginServices.cpp`、`ZeroTierQueries.cpp`、`AppLog.cpp`、入口 `main.cpp`），内嵌 `resources/zt-installer.sh`。
- `plugins/voice-chat/server/`：旧 C++ 语音服务端插件（`.ztplugin`，版本 0.1.0）。
- `tests/`：旧 C++ Package/Query 测试与 Python Integration/Startup/Installer 测试。
- `scripts/build-linux.sh`：旧构建入口，仅在显式使用时执行。
- `scripts/package-server-sdk.py`：旧 C++ SDK 打包脚本。
- `DEPLOYMENT.md`、`README.md`：迁移前的服务端文档，命令与路径均指向旧实现。

## 旧构建命令

需要 Linux、Qt6（Core/Network/Test）与 CMake 3.21+；不要求 Java。根 CMake 默认不构建本目录，只在显式开启时加入：

```bash
cmake -S . -B build/cmake-build-server-old \
  -DZTGUI_BUILD_CLIENT=OFF -DZTGUI_BUILD_SERVER_OLD=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake-build-server-old --target zt-server --parallel
```

也可以独立配置 `src/server-old`：

```bash
cmake -S src/server-old -B /tmp/ztserver-old -DZTSERVER_BUILD_TESTS=ON
```

旧打包入口（输出与正式 Java 包区分）：

```bash
bash src/server-old/scripts/build-linux.sh
# build/dist/linux-x64-cpp/、build/dist/linux-x64-cpp.tar.gz、build/dist/ZTServer-api-cpp-v0.4.zip
# 需要旧语音插件时添加 --build-voice-plugin
```

## 与正式服务的差异

- 正式服务端不加载 `.ztplugin`，改用 Java 插件 `plugins/*.jar`；旧 C++ 插件仅在此归档。
- 安装/卸载/Moon 操作已从宿主 CLI 移到独立的 `zt-installer.sh`，见 [Java 服务端部署说明](../server/DEPLOYMENT.md)。
- `ZTS1`、`builtin` JSON、`ZTD1`、`ZTV1` 线协议在新实现中保持不变，旧客户端无需修改。
- 旧动态库与 Ubuntu 24.04 的 Qt/glibc 绑定；它不能再作为 Ubuntu 22.04 的交付包，移机或回滚时需在匹配环境重新构建。
