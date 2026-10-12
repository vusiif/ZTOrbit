# ZTServer

Linux 无界面 ZeroTier 查询与插件转发服务，C++17 / 动态 Qt6.4 Core、Network。程序版本与客户端共用根 `VERSION`；服务端 SDK 使用独立包 `ZTServerPluginApi`。

## 构建

Ubuntu 24.04：`apt-get install cmake g++ qt6-base-dev python3`。

```bash
cmake -S src/server -B cmake-build-server -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-server --target zt-server --parallel
# 仅明确需要语音插件时开启：
cmake -S src/server -B cmake-build-server -DZTSERVER_BUILD_VOICE_PLUGIN=ON
cmake --build cmake-build-server --target ztserver_voice_chat --parallel
```

`bash scripts/build-linux.sh --build-voice-plugin` 生成 `dist/linux-x64.tar.gz`、条件更新的 `dist/ZTServer-api-v0.3.zip`，以及独立 `dist/server-plugins/ztserver_voice_chat.so`。不带插件开关只编译宿主。运行包不携带测试、插件或运行数据，目标机器需安装匹配的动态 Qt Core/Network 运行库（Ubuntu24.04：`libqt6core6t64 libqt6network6t64`）。

## 启动

```bash
cd /root/Server/ZTServer
./zt-server --server --port 18100 --listen 0.0.0.0
./zt-server --install zerotier --ip 198.51.100.1
./zt-server --install ztncui
./zt-server --moon
./zt-server --uninstall all --yes
```

运行操作要求 root。安装与卸载无目标时显示选择菜单，目标支持 `zerotier/ztncui/all`，短参数为 `-i/-u/-m/-s/-p`。四种操作互斥；无参数显示帮助。`--force` 重建已有 Moon/容器；`--dry-run` 验证并打印安装脚本参数。安装 ZeroTier 同时生成或复用并导出 Moon；仅安装 ztncui 要求已有 ZeroTier。卸载 ZeroTier 会删除节点身份，仅卸载 ZeroTier 时保留 ztncui 配置；全量卸载删除两者。嵌入安装脚本不依赖 `.tmp/`，流程支持 Ubuntu/Debian 与 systemd。

服务保持前台运行，由附带 `zt-server.service` 交给 systemd 托管。启动工作目录决定 `plugins/`、`plugins-data/` 与 `logs/` 的位置；插件变化后需重启。扫描目录直接子文件中的 `.so`，坏插件被拒绝并记录日志。首版没有账户、Token 或 TLS，监听全部 IPv4 地址，访问范围由部署网络和防火墙控制。

SDK、协议和查询接口见 [服务端 API](../../docs/server-api.md)，语音插件部署与业务规则见 [语音服务端文档](plugins/voice-chat/docs/voice-chat.md)。
