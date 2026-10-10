# macOS / Apple Silicon

这里是尚未在 Mac 上验证的平台实现和构建入口，当前没有已编译的 macOS 成品。

## 构建

在 Apple Silicon Mac 上安装 Xcode Command Line Tools、CMake 和支持 arm64 的 Qt 6.8 或更新 SDK。
Qt 6.8 的目标系统为 macOS 12 或更新版本；使用更新 Qt 时，以该 Qt SDK 的系统要求为准。

若使用 Homebrew，可先执行：

```sh
xcode-select --install
brew install cmake qt
```

把项目复制到 Mac 后，在项目目录运行：

```sh
bash scripts/build-macos.sh
```

如果使用 Qt 官方 SDK，而不是 Homebrew：

```sh
QT_ROOT="$HOME/Qt/6.8.3/macos" bash scripts/build-macos.sh
```

脚本只构建 `arm64`，先运行 GUI 回归测试，再生成包含 Qt Frameworks 和插件的独立应用。
输出文件：

- `dist/macos-arm64/ZTGUI-arm64.dmg`
- `dist/macos-arm64/ZTGUI-arm64.app.zip`，解压后是 `ZTGUI.app`

构建脚本使用本机 ad-hoc 签名，不包含 Apple Developer ID 签名或公证。
面向其他用户正式分发前，需要使用自己的 Apple Developer ID 进行签名和公证，并做实机安装验证。

## 平台实现

- macOS 默认安装目录为 `/Library/Application Support/ZeroTier/One`，服务为 `com.zerotier.one`。
- 服务操作通过 `launchctl` 执行；启动或重启会等待本地 API 可用，再提交加入网络请求。
- 安装从 ZeroTier 官方下载 macOS PKG，检查安装包签名，再调用系统 `installer`。
- 卸载先调用安装包提供的 `uninstall.sh`，成功后清除 ZeroTier 数据目录；界面仍要求二次确认。
- GUI 保持普通用户身份，通过 macOS 系统授权窗口运行需要管理员权限的操作。
- 初次读取节点时需要授权。认证文件、端口文件和已签名 Moon 文件缓存到当前 GUI 进程的私有临时目录，CLI 通过 `-D` 指定该目录。认证令牌不放入命令行参数，也不写入日志。
- 服务或 Moon 操作结束后更新临时目录。若通过其他软件修改 Moon 文件，可重开 ZTGUI 更新本地文件缓存。
- Moon 导入、删除和 `.ztbundle` 配置包使用与 Windows 相同的界面；实际文件写入系统目录。
- 日志位于程序可执行文件所在目录的 `logs/YYMMDD-{num}.log`（`.app` 内为 `Contents/MacOS/logs/`），每日从 1 开始，每次启动按已有最大编号递增。该目录需要可写；版本由根目录 `VERSION` 同步到 macOS 应用元数据。
- 中文字体使用 PingFang SC，应用图标在打包时生成 ICNS。

## 首次实机验证

需要在真实 Mac 上确认：系统授权的接受与取消、官方 PKG 安装、服务启动/停止/重启、私有网络加入申请、Moon 导入/导出/删除、移除全部数据，以及 Qt 依赖是否随应用正确部署。
回归测试使用模拟 CLI 和临时数据目录，不操作本机 ZeroTier 服务；Windows 的 PowerShell 专项检查在 macOS 上跳过。

平台依据：[Qt 6.8 macOS 支持](https://doc.qt.io/qt-6.8/macos.html)、[Qt macOS 部署](https://doc.qt.io/qt-6/macos-deployment.html)、[ZeroTier macOS 安装文件](https://github.com/zerotier/ZeroTierOne/tree/dev/ext/installfiles/mac)、[ZeroTier CLI 参数](https://github.com/zerotier/ZeroTierOne/blob/dev/one.cpp)。
