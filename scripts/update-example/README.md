# 更新清单示例

`manifest.example.json` 仅用于说明字段；包大小、SHA-256 和版本均为示例，不能直接发布。真实协议为客户端请求 `/updater`，独立更新程序请求 `/file?version=<版本>`。

将普通发行 ZIP 上传到服务器选定位置，执行 `bash ./update.sh --version <版本> --file <ZIP路径>`；文件参数必填，支持绝对路径和相对当前工作目录的路径。服务器自动转换 ZIP、生成清单并用 root 专用私钥签名，将 `.sig` 作为 `/updater` 响应头 `X-Z2-Signature` 提供。客户端不请求签名文件。

客户端浮窗只输入 IPv4 与端口（默认 18102），空 IP 表示停用；公钥由 `ZTO_UPDATE_PUBLIC_KEY_BASE64` 编译内置。

完整发布命令、文件布局和运维见[更新服务器](../../docs/update-server.md)，字段约束、校验和安装事务见[自动更新协议](../../docs/auto-update.md)。
