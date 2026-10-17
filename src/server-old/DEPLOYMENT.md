# ZTServer 运行说明

运行平台为 Linux/root。此包针对 Ubuntu24.04 x86_64 构建，需安装动态运行库 `libqt6core6t64 libqt6network6t64 libsodium23`。版本以包内 `VERSION` 或 `./zt-server --version` 为准。

## 启动与依赖检查

推荐通过发行包中的启动脚本运行，脚本从自身目录查找程序并检查实际动态库依赖：

```bash
./start-server.sh --check-deps    # 仅检查，不启动
./start-server.sh                 # 检查通过后启动，监听 0.0.0.0:18100
./start-server.sh --server --port 18200  # 显式参数原样传给 zt-server
./start-server.sh --version
```

缺少动态库、运行库版本不匹配或无法检查时，脚本打印诊断并以状态码78退出；Ubuntu24.04 上同时显示安装命令，不自动修改系统。依赖齐全则通过 `exec` 启动程序，保留信号与退出状态。脚本不带参数时默认启动服务；带参数时完整转发，其他操作请传入对应参数。

## 安装运行依赖

在 Ubuntu24.04 目标机器上先安装运行库，再启动程序（root 执行）：

```bash
apt-get update
apt-get install -y libqt6core6t64 libqt6network6t64 libsodium23
./zt-server --version
```

出现 `libQt6Network.so.6: cannot open shared object file` 表示动态加载器找不到 Qt6 Network 运行库，程序尚未进入主函数。发行包不内置这些系统库；只运行服务无需安装 `qt6-base-dev` 或编译工具。

若仍无法启动，执行 `cat /etc/os-release` 和 `ldd ./zt-server`，核对系统版本及输出中的 `not found`。Qt6 运行库位于 Ubuntu 的 universe 仓库，找不到软件包时先检查是否启用该仓库。Ubuntu22.04 的 Qt 版本和包名不同，不能仅替换包名或手工创建 `.so` 软链接来运行本 Ubuntu24.04 包；其他发行版需在对应环境重新构建。

## 服务部署

将主程序包解压到 `/root/Server/ZTServer`；需要语音服务时，将独立发布的 `ztserver_voice_chat.ztplugin` 放入其 `plugins/` 文件夹。主程序包不内置业务插件，宿主只加载 `.ztplugin`，裸 `.so` 是构建中间产物。然后：

```bash
cd /root/Server/ZTServer
install -m644 zt-server.service /etc/systemd/system/zt-server.service
systemctl daemon-reload
systemctl enable --now zt-server.service
systemctl status zt-server.service
```

附带 unit 的 `WorkingDirectory` 和 `ExecStart` 固定使用 `/root/Server/ZTServer`。如果实际解压到 `/root/ZT` 等其他目录，安装 unit 后、启动服务前，将 `/etc/systemd/system/zt-server.service` 中这两处路径改为实际目录，再执行 `systemctl daemon-reload`。插件应放在该工作目录的 `plugins/` 子目录中，直接放在程序旁不会被加载。

附带unit以root运行，通过启动脚本检查依赖并在服务运行失败后重试；依赖检查返回78时停止重试，修复后执行 `systemctl restart zt-server.service`。默认监听全部IPv4地址的TCP/UDP18100，访问由部署网络和防火墙管理。手动运行可使用 `./start-server.sh`；服务保持前台运行，不自行转入后台。

启动脚本固定以自身目录为工作目录，决定 `plugins/`、`plugins-data/` 和 `logs/` 的位置；绕过脚本直接执行 `zt-server` 时仍以调用者工作目录为准。插件仅在启动时扫描，增减后需重启。宿主日志位于 `logs/`，插件显式调用日志接口时也写入 `plugins-data/<插件名称>/logs/`。更新程序时保留这些数据目录。

从旧独立语音服务迁移时，先停止并禁用 `ztgui-voice.service`，确认18100空闲，再启动新服务。须同时更新客户端语音插件包；旧外层协议不兼容。回滚到旧服务时，停止并禁用 `zt-server.service`，恢复与旧服务匹配的旧客户端版本及插件，再启用旧语音unit；原 `/opt/ztgui-voice` 与unit应保留。

## 安装管理

```bash
./zt-server --install zerotier --ip 198.51.100.1
./zt-server --install ztncui
./zt-server --moon
./zt-server --uninstall ztncui
./zt-server --uninstall all --yes
```

`--install/-i` 与 `--uninstall/-u` 支持 `zerotier/ztncui/all`，无目标时显示选择菜单。`--moon/-m` 导出Moon，`--server/-s` 启动服务，`--port/-p` 修改服务端口。四种操作互斥；无参数显示帮助。

安装ZeroTier时自动生成或复用并导出Moon；仅安装ztncui要求已有ZeroTier。Moon导出到执行命令时的当前目录。`--ip` 指定公网IPv4，`--force` 重建已有Moon或容器。安装流程只支持Ubuntu/Debian和systemd。

卸载ZeroTier删除节点身份，重新安装后节点ID会改变；仅卸载ZeroTier保留ztncui配置，全量卸载删除两者。卸载需交互确认，自动化执行显式加`--yes`。`--dry-run` 只验证并打印安装脚本参数。

第三方开发使用单独的 `ZTServer-api-v<API版本>.zip`；归档 SDK 与客户端 SDK 版本各自独立，动态插件接口独立，不能混载。
