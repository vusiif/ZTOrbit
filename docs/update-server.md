# Z2Updater 部署与发布

更新服务位于 `root@10.10.10.1:/root/Server/Z2Updater/`，由 `z2-updater.service` 管理 Nginx，开机启动，只监听 `10.10.10.1:18102`。它独立于 Z2Hub。网络边界见[网络部署约定](network-conventions.md)，客户端协议见[自动更新](auto-update.md)。

## 发布：上传 ZIP，再执行 update.sh

先按开发者分配的根 `VERSION` 编译并打包，使用干净的 `build/dist/Z2-windows-x64.zip`。将这个普通发行 ZIP 直接上传：

```powershell
scp build/dist/Z2-windows-x64.zip root@10.10.10.1:/root/Server/Z2Updater/example.zip
```

上传完成后，在服务器执行（示例版本必须与 ZIP 内 `Z2.exe` 的实际版本一致）：

```bash
cd /root/Server/Z2Updater
bash ./update.sh --version 0.5.3 --file example.zip
```

脚本会自动检查 ZIP 和 PE 版本、去掉发行 ZIP 的外目录、生成更新包及 SHA-256、生成并签名版本信息，校验 Nginx 配置并平滑切换 `/updater` 与 `/file`，最后通过 HTTP 确认新版本已经生效。

`--file` 必填，不预设文件名或上传目录。相对路径以执行命令时的当前工作目录为准，也支持绝对路径；路径含空格时加引号。

- 相同版本、相同文件内容可重复执行，不改写已发布包和签名元数据。
- 相同版本但内容不同会拒绝发布，需由开发者分配新版本并重新编译；改名或修改清单不能绕过版本检查。
- 客户端只安装严格更新的版本，因此要分发新的修改，也必须使用更高版本。
- 包无效或版本不匹配时不会切换；激活失败会恢复原接口配置。并发发布通过文件锁互斥。
- 指定的原始 ZIP 保留；发布过程中使用的暂存目录会自动清理。

可选参数：

```bash
# 仅检查包、版本、密钥及同版本一致性，不发布
bash ./update.sh --version 0.5.3 --file example.zip --check

# 指定任意位置的 ZIP，路径含空格时加引号
bash ./update.sh --version 0.5.4 --file "/root/Downloads/Z2 0.5.4.zip"

# 本次迁移要求的最低可升级版本
bash ./update.sh --version 0.6.0 --file example.zip --min-version 0.6.0
```

默认最低可升级版本和接口地址保存在 `conf/publish.json`：

```json
{"baseUrl":"http://10.10.10.1:18102","minUpdatableVersion":"0.5.3"}
```

当前发布版本可通过 `GET /updater` 查询。发布脚本从 ZIP 校验客户端版本；版本号由开发者在仓库中维护。

## 接口与文件

| 接口 | 用途 |
| --- | --- |
| `GET /updater` | JSON 版本信息；`X-Z2-Signature` 为 Ed25519 签名；禁止缓存 |
| `GET /file?version=<版本>` | 下载指定版本的 Windows x64 更新 ZIP |
| `GET /file` | 重定向到当前版本的下载地址 |
| `GET /healthz` | 健康检查 |

指定版本的更新包保持不可变。非法或不存在的版本返回 404；只允许 GET/HEAD，不提供 HTTP 上传、发布命令或目录列表。发布时绑定同一份版本信息和签名头，客户端下载带版本参数的包，避免版本切换造成错配。

服务器目录：

- `update.sh`：发布入口；仓库源文件为 `scripts/update-server/update.sh`。
- `uploads/`：可选的上传暂存目录，不限制 `--file` 的文件位置。
- `tools/publish-update.py`：发布实现，源文件为 `.agents/skills/zto-build/scripts/publish-update.py`。
- `tools/update-manifest.py`：Ed25519 签名工具。
- `conf/`：Nginx 配置、公钥、发布参数及当前接口配置。
- `secrets/update-signing.key`：签名私钥；目录权限 700，文件权限 600，归 root 所有。
- `work/`：发布暂存区，仅 root 可访问，完成或失败后清空本次临时目录。
- `www/api/windows-x64/<version>/`：签名元数据。
- `www/z2/releases/windows-x64/<version>/`：客户端更新 ZIP。

Nginx 由动态非特权用户运行，部署目录只读映射为 `/srv/z2-updater/`。Web 进程没有读取私钥的权限，HTTP 也不提供密钥目录。发行版默认 `nginx.service` 保持 masked；常驻服务仍只有 Nginx，Python 只在执行发布命令时运行。

## 密钥与客户端接入

现有公钥保持不变：`qe3kJlxS6qcw0xbfBKaM929qmh9VL6jRw8eC6Wmo+/g=`。

为实现服务器端“只上传 ZIP 后发布”，沿用的签名私钥已配置到服务器 root 专用目录，服务器负责签名。本地 `build/update-keys/` 中的原始密钥仍保留作备份，不进入 Git。不要删除唯一的密钥副本；丢失或更换私钥会影响现有客户端的信任关系。

客户端构建使用 `ZTO_UPDATE_PUBLIC_KEY_BASE64` 内置上述公钥。用户在设置页点击“更新服务器”，输入 IP `10.10.10.1`、端口 `18102` 并保存。IP 留空保存即停用检查；没有预设地址或回退源。

## 运维与验证

```bash
systemctl status z2-updater.service
journalctl -u z2-updater.service -n 50 --no-pager
curl --noproxy '*' -f http://10.10.10.1:18102/healthz
curl --noproxy '*' -fi http://10.10.10.1:18102/updater
curl --noproxy '*' -fIL http://10.10.10.1:18102/file
```

访问日志为 `/var/log/z2-updater/access.log`。Unit 安装于 `/etc/systemd/system/z2-updater.service`；修改后需 `systemctl daemon-reload`。发布程序的隔离回归入口为 `python3 src/tests/update-server/PublishTests.py`，使用临时目录、测试密钥及本机已构建客户端的 PE 元数据，不触碰真实发布目录。
