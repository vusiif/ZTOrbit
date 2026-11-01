# ZTOrbit

## 项目概述

客户端发行名为 **Z2**，服务端发行名为 **Z2Hub**；代码继续使用 `zto`／`ztohub`，产物命名见[命名约定](docs/naming.md)。

|              |        客户端        |          服务端           |
| :----------: | :------------------: | :-----------------------: |
| **运行环境** |     windows x64      |    Ubuntu 24.04 x86_64    |
|  **技术栈**  | C++17、Qt6 Quick/QML |      Java 25、Netty       |
| **端口使用** |       9993/UDP       | 9993/UDP、9443/TCP、18100 |

- 网络部署与默认连接行为统一遵循[网络约定](docs/network-conventions.md)，约定之外的配置由部署者自行负责。
- 设置页可直接管理本机 ZeroTier `local.conf` 的两个 TCP 回退开关和中转服务器，并可按网络选择符合 Moon 约定的 `9443/TCP` 中继；规则见[本机 ZeroTier TCP 回退配置](docs/local-config.md)。维护与验证范围见[维护约定](docs/maintenance.md)。

## 仓库结构

```text
ZTOrbit/
 |--VERSION               客户端程序版本；客户端 API 取前两段数字和同一后缀
 |--SERVER_VERSION        客户端指定的服务端程序版本；服务端 API 同规则派生
 |--.agents/              AI开发相关文档和工具
 |--docs/                 详细文档
 |--scripts/              构建脚本
 |--src/
     |--shared/
     |--tests/
     |--client/
     |   |--api/
     |   |--main/
     |   |--assets/
     |--server/
     |   |--api/
     |   |--main/
     |   |--scripts/
     |--plugins/          一些插件，后续会独立成一个仓库
```

## 编译

| 目标 | 环境要求 |
| --- | --- |
| Windows 客户端 | Qt 6.8+ 动态库、CMake 3.21.1+、MSVC 2022（C++17） |
| Linux 服务端 | JDK 25、Maven 3.9+ |

在仓库根目录执行以下命令。

### Windows 客户端

将 `$qtSdk` 替换为本机 Qt SDK 路径：

```powershell
$qtSdk = 'C:/Qt/6.8.3/msvc2022_64'
./scripts/build-windows.ps1 -QtRoot $qtSdk
```

### Linux 服务端

```bash
bash scripts/build-linux.sh
```

- [macOS 构建说明](docs/macos.md)
- [编译与打包 SKILL](.agents/skills/zto-build/SKILL.md)
- [服务端部署说明](src/server/DEPLOYMENT.md)

## 插件编写

| 项目 | 客户端插件 | 服务端插件 |
| --- | --- | --- |
| 项目目录 | `src/plugins/<插件名>/client/` | `src/plugins/<插件名>/server/` |
| API 文档 | [客户端 API](docs/plugin-api.md) | [服务端 API](docs/server-api.md) |

客户端插件开发：

1. 构建或引用客户端插件 SDK。
2. 创建插件项目，引用 SDK 并编写插件。
3. 编译并打包为 `.z2plugin`，在客户端中导入。

服务端插件开发：

1. 构建或引用服务端插件 SDK。
2. 创建插件项目，引用 SDK 并编写插件。
3. 编译为 JAR，放入服务端运行目录的 `plugins/`，重启服务端。

完整步骤见[插件开发 SKILL](.agents/skills/zto-plugin-dev/SKILL.md)。
