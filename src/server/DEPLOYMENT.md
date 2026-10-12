# ZTServer 运行说明

运行平台为 Linux/root。此包针对 Ubuntu24.04 x86_64 构建，需安装动态运行库 `libqt6core6t64 libqt6network6t64`。版本以包内 `VERSION` 或 `./zt-server --version` 为准。

## 服务部署

将包解压到 `/root/Server/ZTServer`，将独立发布的 `ztserver_voice_chat.so` 放入其 `plugins/` 文件夹，然后：

```bash
cd /root/Server/ZTServer
install -m644 zt-server.service /etc/systemd/system/zt-server.service
systemctl daemon-reload
systemctl enable --now zt-server.service
systemctl status zt-server.service
```

附带unit以root运行并在启动失败后重试。默认监听全部IPv4地址的TCP/UDP18100，访问由部署网络和防火墙管理。直接运行可使用 `./zt-server --server --listen 0.0.0.0 --port 18100`；服务保持前台运行，不自行转入后台。

启动工作目录决定 `plugins/`、`plugins-data/` 和 `logs/` 的位置。插件仅在启动时扫描，增减后需重启。宿主日志位于 `logs/`，插件显式调用日志接口时也写入 `plugins-data/<插件名称>/logs/`。更新程序时保留这些数据目录。

从旧独立语音服务迁移时，先停止并禁用 `ztgui-voice.service`，确认18100空闲，再启动新服务。须同时更新客户端语音DLL；旧外层协议不兼容。回滚时停止并禁用 `zt-server.service`，恢复旧客户端DLL，并启用旧语音unit；原 `/opt/ztgui-voice` 与unit应保留。

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

第三方开发使用单独的 `ZTServer-api-v<API版本>.zip`；SDK和客户端SDK版本数字一致，但动态插件接口独立，不能混载。
