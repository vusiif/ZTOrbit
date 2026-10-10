# ZTGUI

基于 Qt 6 Widgets 的 ZeroTier 管理工具。Windows 版本已构建验证，Apple Silicon macOS 平台实现与打包脚本已准备，尚待 Mac 实机验证。

## 使用

- 程序与侧栏标题为“ZTGUI”，导航仅保留“ZeroTier”。右侧合并节点信息、服务操作、网络和成员列表，内容超出窗口时通过整页纵向滚动浏览。侧栏宽度可拖动调整，网络与成员之间没有分隔线。
- 侧栏标题左侧的圆点表示节点在线状态：绿色在线，红色离线。ZeroTier 页面显示可点击复制的节点 ID，以及 `v1.16.2` 形式的 ZeroTier 版本。
- 主页按 `zerotier-cli info` 的状态规则判断异常：`OFFLINE` / `ONLINE` 不显示提示，`TUNNELED` 等其他状态显示红色“服务异常，请尝试重启。”；每 1 秒刷新，恢复后自动隐藏。
- 侧栏底部齿轮向右上方展开设置。主题默认跟随系统，也可通过滑动按钮切换浅色 / 深色，或重新勾选“跟随系统”。“插件”暂未启用，“关于”显示 GUI 与 Qt 版本。
- 按钮、输入框和导航去除系统焦点虚线框，保留键盘输入和操作。
- 界面动画始终启用，包含页面、主题、菜单和浮窗切换。耗时操作期间显示加载遮罩，防止重复提交。
- ZeroTier 页面的服务按钮读取 Windows 服务状态：服务不存在时显示“安装”；存在但未启动时显示“启动”，下拉菜单提供“卸载”；运行时显示“停止”，下拉菜单依次提供“重启”和“卸载”。
- 安装通过 winget 完成。卸载需要确认，成功后删除 `%ProgramData%\ZeroTier` 中的节点身份和网络配置。MSVC 发行版启动时请求管理员授权。
- 合并页面先显示网络列表，再显示成员列表。全部列宽可拖动表头分隔线调整。列表每 1 秒异步更新，保留选择、滚动位置、列宽、展开状态和备注输入；服务不可用时保留上次数据。
- 网络列表内最后一行的“加入网络”打开浮窗：输入 16 位网络 ID，回车或点击“确认”加入，Esc 关闭。左下方“导入中转站”可选择 `.ztbundle` 配置包，自动配置 Moon、重启服务并加入包内网络，无需手动输入 ID。也兼容单独 `.moon` 文件，单独导入时仍需手动输入网络 ID。
- 双击网络行展开空的成员子列表，后续可用于显示该网络下的成员。当前客户端不支持主动发现，因此暂不填充。
- 网络行右键提供“修改备注”“复制ID”“本机ip”“退出网络”。复制 IP 时去掉 CIDR 掩码，多个 IP 以换行分隔。备注默认显示网络名称，留空恢复默认名称。
- 如果存在文件名后十位与网络 ID 前十位相同的 Moon 文件，网络右键菜单会在“本机ip”和“退出网络”之间增加“导出中转站”，导出包含完整网络 ID、Moon 原始文件名及签名文件内容的 `.ztbundle` 配置包，供另一台电脑直接导入。包格式带版本号，导入前校验网络 ID、文件名匹配和文件内容编码。该匹配适用于由同一服务器 ztncui 创建网络、并由该服务器 ZeroTier 创建和签名 Moon 的部署。
- 网络列表右下方“中转站”打开 Moon 浮窗。列表仅显示名称，双击打开完整详情，右键提供“备注”“详细”“删除”。删除文字显示红色，需二次确认。
- 导入配置包会先复制 Moon 并重启服务，成功后提交加入网络请求；私有网络仍需要控制端授权。如果 Moon 配置失败，不提交加入请求；若加入失败，保留已配置的 Moon 和网络 ID，允许重试。
- “添加Moon文件”支持多文件选择，Moon 浮窗打开时也支持拖入 `.moon` 文件。文件复制到官方 `moons.d` 目录，导入和删除后重启服务。
- 成员隐藏节点 ID 列，将根节点排在最后，其他成员依次按直链、中转、不可达排序。右键菜单第一项是节点 ID，点击复制；“修改备注”在名称列内编辑，回车或左键点击其他位置保存。
- 成员 IP 去掉连接路径中的端口；没有路径显示“—”。保留原有不可达 LEAF 过滤行为，根节点和 Moon 仍显示。
- 停止服务时忽略命令错误；重启始终继续尝试启动，启动失败会提示并记录。程序启动后在程序所在目录创建 `logs/`，以 `YYMMDD-{num}.log` 保存本次运行日志，例如 `261010-1.log`。同一天按已有最大编号加一，每次启动创建新文件；新的一天从 1 开始，不覆盖历史日志。运行期间跨天仍写入本次启动的文件，日志立即刷新到磁盘。

## 版本与目录

主程序当前版本为 `0.2.2`，API 版本为 `0.2`。根目录 `VERSION` 是唯一版本来源，“关于”、启动日志、Windows 文件属性和 macOS 应用版本均从中读取，API 常量和 SDK 包版本由前两段生成。正式发布前版本规则为 `0.<核心版本>.<后缀版本>`：首段固定为 0，当前核心版本为 2，同时对应 API `0.2`；非核心功能更新、修复和优化只递增后缀。核心/API 版本变更需明确决定，并同步修改第二段、将后缀归零。每次更新同时维护 `CHANGELOG.md`，详细规则见 `AGENTS.md`。

```text
src/api/      独立插件 SDK（公开头文件、生成的 API 版本、CMake 包）
src/main/     主程序源码（含 tabs/、plugins/），依赖 API SDK
src/plugins/  独立插件项目，每个插件一个子目录
.agent/skills/ 项目内插件开发 SKILL
tests/        回归测试及模拟程序
assets/       图标和 Windows 资源模板
scripts/      构建与打包脚本
docs/         平台及插件 API 文档
VERSION       程序版本的唯一来源
CHANGELOG.md  更新记录
dist/         编译与打包产物
```

## 插件 API

左侧菜单扩展接口已接入主窗口，支持插件注册／注销、按稳定 ID 导航、多菜单、懒创建页面和资源清理。接口定义、生命周期约定及接入示例见 [插件 API v0.2](docs/plugin-api.md)。当前提供进程内 C++ API，外部插件加载和插件管理界面尚未实现。

API SDK 可单独通过 `cmake -S src/api -B cmake-build-api` 配置，再用 `cmake --install cmake-build-api --prefix dist/ZTGUI-api-v0.2` 安装；配置时需指定 Qt SDK。插件项目通过 `find_package(ZTGUIPluginApi 0.2 EXACT CONFIG REQUIRED)` 和 `target_link_libraries(my_plugin PRIVATE ZTGUI::Api)` 引用，详细步骤见接口文档。

编写插件前可使用项目内 [ztgui-plugin-dev SKILL](.agent/skills/ztgui-plugin-dev/SKILL.md)，默认在 `src/plugins/myplugin/` 创建插件项目。

## 编译

Apple Silicon macOS 的准备与构建步骤见 [macOS 构建说明](docs/macos.md)，在 Mac 上运行 `bash scripts/build-macos.sh`，生成 `.dmg` 与包含 `.app` 的 ZIP。

需要 Qt 6、CMake 和支持 C++17 的编译器。Windows 推荐 MSVC 2022。

```powershell
cmake -S . -B cmake-build-release -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --build cmake-build-release --config Release --parallel
C:/Qt/6.8.3/msvc2022_64/bin/windeployqt.exe --release cmake-build-release/Release/ZTGUI.exe
```

### 单 EXE 编译

使用以 `-static -static-runtime -release` 编译的 Qt 6 SDK，再构建本项目：

```powershell
cmake -S . -B cmake-build-single -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/static"
cmake --build cmake-build-single --config Release --parallel
```

静态 Qt SDK 会自动启用静态 MSVC 运行库，并将平台插件链接进程序。生成的 EXE 不需要运行 `windeployqt`，也不需要在旁边放置 Qt 或 MSVC DLL。ZeroTier 服务仍需要单独安装，可通过程序中的 winget 安装入口完成。

## 回归测试

```powershell
cmake -S . -B cmake-build-release -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" -DZEROTIER_BUILD_TESTS=ON
cmake --build cmake-build-release --config Release --parallel
$env:PATH = "C:/Qt/6.8.3/msvc2022_64/bin;" + $env:PATH
$env:QT_PLUGIN_PATH = "C:/Qt/6.8.3/msvc2022_64/plugins"
ctest --test-dir cmake-build-release -C Release --output-on-failure
```

测试使用独立的模拟 CLI 和临时设置，不调用系统 ZeroTier。卸载清理测试只在构建目录下生成的临时目录中执行，检查失败时保留数据、成功后删除数据。日志位于构建目录的 `test-results.txt`，界面截图位于 `screenshots/`。测试用的 `zerotier-cli.exe` 不能打包进正式发行目录。
