# 桌面共享控制与媒体金样例

本目录的 JSON 是客户端 C++ 与服务端 Java 共用的协议金样例。两端的测试都直接读取这些文件，不各自维护一份期望值。

当前控制样例为 28 条合法、34 条非法，媒体样例为 10 条合法、29 条非法。控制格式覆盖 `h264` 和 `bgra-tiles`；分块初始化数据必须为空。ZDS1 外层与 1232 字节 UDP 上限不变，分块内层 ZTD1 由[媒体分块文档](../../../media/client/docs/tiles.md)定义。

## `control-golden.json`

- `format`：`zds-control-golden/1`。
- `valid[]`：合法消息。
  - `kind`：`request`、`response` 或 `event`。
  - `op`：请求操作名；响应使用它选择期望的响应类型。事件样例使用 `event`。
  - `bytes`：规范紧凑 UTF-8 JSON，也是逐字节比较的期望值。
  - `expect`：解码后的关键字段，测试按字段名单独断言。
- `invalid[]`：非法消息。
  - `bytes` 为 UTF-8 文本；无法表示为文本的输入使用 `bytesHex`。
  - `error`：期望的错误码，例如 `PROTOCOL_VERSION`、`INVALID_REQUEST`、`UNKNOWN_OPERATION`。

规范编码规则：

- 请求键顺序固定为 `v,id,op,data`；成功响应为 `v,id,ok,data`；错误响应为 `v,id,ok,error`；事件为 `v,event,data`。
- 对象字段按协议表顺序输出；数字保持整数词法形式，不使用浮点往返。
- 64 bit ID、流代次和统计值使用十进制字符串；token 与初始化数据使用小写十六进制字符串。
- 非 ASCII 文本直接以 UTF-8 输出，不额外转义。

## `media-golden.json`

- `format`：`zds-media-golden/1`。
- `valid[]`：
  - `bytesHex`：完整业务负载，即 88 字节大端固定头加视频片段（注册包只有固定头）。
  - `expect`：头部字段；`pts`/`dts` 为有符号十进制，其余 64 bit 字段为无符号十进制。
  - `zts1DatagramBytes`：在 `desktop-share` 路由下封装的完整 UDP 报文长度；`1125 + 88 + 19 = 1232` 是边界样例。
- `invalid[]`：`bytesHex` 与期望的媒体错误码，例如 `TOO_LONG`、`BAD_FRAGMENT_COUNT`、`BAD_ROUTE`。

C++ 测试另外验证 ZTS1 外层路由、半包/粘包、1 MiB 编码包拆分（933 片，末片 76 字节）和错误路由；Java 外层由宿主处理，因此只验证业务负载与 `datagramBytes()` 计算。

## 运行测试

```powershell
# C++（需要安装后的 API SDK 与 Qt）
cmake -S src/tests/desktop-share -B build/cmake-build-tests-desktop-share `
    -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_PREFIX_PATH=$apiSdk;$qtSdk"
cmake --build build/cmake-build-tests-desktop-share --config Release --parallel
build/cmake-build-tests-desktop-share/Release/desktop_share_protocol_tests.exe
```

```powershell
# Java
mvn -f src/plugins/desktop-share/server/pom.xml test
```

修改样例前先确认两端编码器仍能对同一字节序列重新编码出完全相同的结果；样例是契约，不是某一端生成的快照。
