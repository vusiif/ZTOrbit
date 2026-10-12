# 语音插件与服务端

## 架构与边界

`src/plugins/voice-chat/client-plugin/` 是独立 Qt Widgets 动态插件，`server/` 是独立无界面 CMake 服务端，双方共用父目录的 `Protocol.h`。服务端不引用主程序、插件 SDK、Multimedia 或 Opus；客户端只引用公开 SDK，不包含宿主实现。

客户端首次地址为空，端口默认 TCP/UDP `18100`。打开面板或填写地址均不联网；点击连接后才建立会话。上次地址、昵称和设备偏好会保存，但不会自动连接。当前部署目标 `10.10.10.1` 不写入客户端默认值。TCP 和 UDP 均直接连接，不使用系统 HTTP/SOCKS 代理。

房间开放且临时，创建者直接加入，一个会话只能加入一个房间。最后成员离开后删除，服务重启清空。默认最多32个房间、每房间16人、256个连接。首版面向 ZeroTier / 可信内网，无账户、密码、房主控制、TLS、录音、回声消除和自动重连。

加入房间并完成 UDP 注册后使用 `QAudioSource` / `QAudioSink`；设备原生 PCM 格式转换为单声道48 kHz，Opus 编码为20 ms、24 kbps。服务端仅转发，客户端按发送者维护解码器、60 ms缓冲和丢包补偿，混音后限幅输出。设备变化时重新打开音频，断线或退出立即停止并清空会话。麦克风静音不发送音频，耳机静音仍维护接收流。

## 启动预处理与首次页面卡顿

语音客户端、共享协议和服务端类型位于 `vusiif::ztgui::voice_chat`。插件实现 SDK 的 `vusiif::ztgui::api::PluginInterface` 和 `PluginLifecycleInterface`，只在 `onCreate(util::HostServices&, const util::CancellationToken&)` 返回设备初始化任务；工具类型属于 `vusiif::ztgui::util`。启动时先注册菜单并置灰，后台初始化 Qt Multimedia 后端及枚举输入/输出设备，完成后启用菜单。关闭时在枚举前、输入枚举后和输出枚举后检查取消请求，取消时不发布部分设备快照；单次Qt设备枚举不可强行中断，宿主异步等待它返回，GUI继续响应。该步骤不联网，也不启动录音或播放设备；连接服务器和入房仍由用户操作触发。页面只读取加锁的设备快照，设备变化时在会话工作线程重新枚举，再通过信号更新下拉列表。

Windows x64 / Qt6.8.3 本机诊断记录：旧实现DLL加载约3 ms，首次页面工厂约4012 ms，再次创建约4 ms；独立测量首次输出设备查询约4134 ms，先枚举设备后页面创建约10 ms。瓶颈是旧页面构造在GUI线程调用 `QMediaDevices::audioOutputs()`，触发后端和系统音频设备冷初始化。`QMediaDevices` 对象本身构造约0 ms；连接设备变化信号也可能触发懒初始化，因此页面仅在缓存准备好后安装监听。

数据为本机观测，设备/驱动不同会改变耗时。Qt6.8.3 的设备查询调用平台媒体集成层，其getter支持跨线程使用：[Qt源码](https://github.com/qt/qtmultimedia/blob/v6.8.3/src/multimedia/qmediadevices.cpp)。任务完成前宿主GUI保持响应；关闭程序时等待预处理结束，避免后台仍在执行插件代码时卸载动态库。

新实现通过实际宿主加载器再次测量：启动预处理约4495 ms，期间10 ms的GUI心跳触发448次，首次页面创建约10 ms，第二次约4 ms。耗时已移出GUI线程，初始化成本仍存在，完成前菜单暂不可点击。

运行期间仍不扫描或热增删插件，安装、移除和更新需退出后调整 `plugins/`，再重启。

## 独立构建与打包

Windows 宿主和插件需要匹配的动态 Qt 6.8+（Widgets、Network、Multimedia）、MSVC 2022 x64、Release。在仓库根执行：

```powershell
$qtSdk = 'C:/Qt/6.8.3/msvc2022_64'
cmake -S src/api -B cmake-build-api -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$qtSdk"
cmake --install cmake-build-api --prefix dist/ZTGUI-api-v0.3
$apiSdk = (Resolve-Path dist/ZTGUI-api-v0.3).Path
cmake -S src/plugins/voice-chat/client-plugin -B cmake-build-voice-client -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$apiSdk;$qtSdk"
cmake --build cmake-build-voice-client --config Release --parallel
```

客户端产物：`cmake-build-voice-client/Release/ztgui_voice_chat.dll`。SDK/API版本为`0.3`，插件自身版本为`0.1.0`。插件依赖固定 Opus `1.6.1` 源码下载并验证 SHA256；离线构建可传入 `-DFETCHCONTENT_SOURCE_DIR_OPUS=/path/to/opus-1.6.1`。Opus 静态链接进插件，宿主仍使用动态 Qt。

明确要求构建语音插件时，执行 `./.agent/skills/ztgui-build/scripts/build-windows.ps1 -QtRoot $qtSdk -BuildVoicePlugin`。脚本默认只构建并压缩主程序，API 变化时更新 SDK，不自动编译插件。指定语音插件后，主程序包部署 Qt/Multimedia/FFmpeg及编译器运行依赖，插件另放在 `dist/plugins/ztgui_voice_chat.dll`，许可证为同目录的 `ztgui_voice_chat-LICENSE.txt`，没有额外嵌套目录。安装时将 DLL 复制到 `ZTGUI.exe` 旁的 `plugins/`，增减插件后重启即可；测试可执行文件和模拟 CLI 不进入发行包。

脚本先部署到干净暂存目录，完成后替换发行目录；旧目录保留为同级 `windows-x64.previous-<随机编号>` 备份，其中的日志、插件数据及额外插件不会带入新包。语音 UDP 初始化失败提示使用服务器返回的实际 UDP 端口，可据此检查自定义端口配置。

Linux 服务端最低 Qt 6.4，Ubuntu24.04的系统包可直接构建，无需客户端 SDK：

```bash
sudo apt-get install cmake g++ qt6-base-dev
cmake -S src/plugins/voice-chat/server -B cmake-build-voice-server -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-voice-server --parallel
sudo cmake --install cmake-build-voice-server --prefix /opt/ztgui-voice
```

必须同时保留服务端上一级的 `Protocol.h`；可复制整个 `src/plugins/voice-chat/` 项目用于构建。服务默认监听 `127.0.0.1`，部署时指定目标内网地址：

```bash
/opt/ztgui-voice/bin/ztgui-voice-server --listen 10.10.10.1 --tcp-port 18100 --udp-port 18100
sudo install -m644 src/plugins/voice-chat/server/ztgui-voice.service /etc/systemd/system/ztgui-voice.service
sudo systemctl daemon-reload
sudo systemctl enable --now ztgui-voice.service
sudo systemctl status ztgui-voice.service
journalctl -u ztgui-voice.service
```

systemd 使用动态非特权用户并自动重启；若 ZeroTier 地址尚未就绪，绑定失败后5秒重试。防火墙需允许目标内网接口的 TCP 和 UDP `18100`。`--max-rooms`、`--max-members`、`--max-connections` 可调整容量。

## 协议 v1

TCP：每条 UTF-8 JSON 后接换行，单条最大64 KiB。请求形式为 `{v:1,id:<整数>,op:<操作>,data:<对象>}`；响应为 `{v:1,id:<对应值>,ok:<布尔>,data:<结果>}`，失败为 `error:<错误码>`。支持拆包和粘包。

| 操作 | 请求 data | 成功 data |
| --- | --- | --- |
| `hello` | `nickname` | `session`、`udpPort`、`rooms` |
| `rooms` | 空对象 | 房间数组 |
| `create` | `name` | `room`快照、十六进制`token` |
| `join` | 数字`room` ID | `room`快照、十六进制`token` |
| `leave` | 空对象 | 空对象 |
| `mute` | 布尔`muted` | 空对象 |
| `ping` | 空对象 | 空对象 |

房间列表对象含 `id,name,members,capacity`；单房间快照含 `id,name,members`，其中成员含 `id,nickname,muted`。服务端事件为 `{v:1,event:"rooms"或"members",data:<列表或快照>}`。客户端5秒心跳；服务端与客户端均以15秒无控制响应判定超时。

UDP：最大1200字节，整数为大端；头部固定30字节：

| 偏移 | 字节数 | 内容 |
| --- | --- | --- |
| 0 | 4 | ASCII `ZTV1` |
| 4 | 1 | 版本1 |
| 5 | 1 | 类型：1注册、2确认、3音频 |
| 6 | 16 | 房间语音凭据 |
| 22 | 4 | 发送者会话ID |
| 26 | 4 | 帧序号，允许32位回绕 |
| 30 | 可变 | 单个20 ms Opus包，仅音频类型携带 |

入房时服务器生成随机凭据，退房立即作废。UDP注册要求来源IP与TCP连接一致，首次有效注册绑定端口，确认返回原会话ID/凭据。音频仅接受已绑定端点和有效房间成员，转发时将凭据替换为接收者凭据，发送者ID保持不变，不回传发送者。客户端每秒重试注册，5次未确认显示不可达，退出房间后可重试。

控制请求每会话每秒最多30次，UDP最多100包；发送积压超过256 KiB关闭慢连接。音频队列、每发送者抖动缓冲和播放积压均有上限，过期包丢弃以避免延迟持续增加。日志不记录语音负载与凭据。

## 验证

自动化测试位于 `tests/plugins/` 和 `tests/voice-chat/`。音频测试需以 `-DVOICE_BUILD_TESTS=ON` 配置客户端，然后运行 `ctest --test-dir cmake-build-voice-client -C Release --output-on-failure`。宿主配置 `-DZEROTIER_BUILD_TESTS=ON -DZTGUI_VOICE_PLUGIN_PATH=/absolute/path/to/ztgui_voice_chat.dll` 可测试真实插件面板；`ZTGUI_LIVE_VOICE_HOST` 可指定已部署服务测试真实房间操作，测试以麦克风静音运行。

```bash
python3 tests/voice-chat/ServerTests.py --binary /path/to/ztgui-voice-server
python3 tests/voice-chat/ServerTests.py --host 10.10.10.1 --port 18100
```

独立服务测试包括容量和15秒超时清理；已部署服务测试只创建临时测试房间，不修改容量和服务状态。音频测试验证合成音频、PCM转换、流式重采样、序号回绕、乱序和丢包。真实麦克风/耳机通话效果及macOS仍需实机验收。
