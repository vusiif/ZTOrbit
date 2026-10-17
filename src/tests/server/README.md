# 服务端测试

新 Java 宿主的黑盒与启动脚本测试。测试不接触真实 ZeroTier：`IntegrationTests.py` 在本机启动 mock HTTP 查询服务，并使用 fixture 插件 JAR 校验路由、插件生命周期、日志和错误路径。

## 前置条件

- Linux 或 WSL，JDK 25、Maven 3.9+、Python 3.10+。
- 先构建宿主与 fixture JAR：

```bash
revision="$(tr -d '\r\n' < SERVER_VERSION)"
api_version="$(printf '%s' "$revision" | sed -E 's/^0\.([0-9]+)\.[0-9]+(-[0-9A-Za-z.-]+)?$/0.\1\2/')"
mvn -f src/server/pom.xml -Drevision="$revision" -DapiVersion="$api_version" clean verify
python3 src/tests/server/build-fixtures.py \
  --api "src/server/api/target/ztserver-api-$api_version.jar" \
  --output /tmp/zt-fixtures
```

## IntegrationTests.py

```bash
# fat jar，自启动临时服务器
python3 src/tests/server/IntegrationTests.py \
  --binary src/server/main/target/zt-server.jar --fixtures /tmp/zt-fixtures

# 通过发行启动脚本（需要同时指定 jar）
python3 src/tests/server/IntegrationTests.py \
  --binary src/server/scripts/start-server.sh \
  --jar src/server/main/target/zt-server.jar --fixtures /tmp/zt-fixtures

# 已部署服务器（不启动本地进程）
python3 src/tests/server/IntegrationTests.py --host 10.10.10.1 --port 18100 --fixtures /tmp/zt-fixtures
```

覆盖：TCP 拆包/粘包/多路由、未知与未就绪路由错误、UDP 回显/未知丢弃/超长丢弃、非法帧断开、限速关闭、fixture 生命周期标记、宿主/插件双写日志、非法 manifest/重复 ID/错误 API/错误 mainClass 拒绝、mock `info` 查询、`controller.members` 参数校验、CLI 退出码与旧安装参数提示、token 不落日志。

`fixtures/FixturePlugin.java` 由 `build-fixtures.py` 编译成多份 JAR，行为由各自 manifest ID 决定：`test-echo`/`test-other` 回显，`test-failed` 启动失败，`test-cancellable` 记录取消/停止标记，`test-mismatch` 的 `info()` 与 manifest 不一致。

## StartupTests.py

```bash
python3 src/tests/server/StartupTests.py
```

用假 `java` 验证 `start-server.sh` 的参数转发、工作目录、退出码、`--check-deps`、缺 Java/旧 Java/缺 JAR 的 78 退出和 `JAVA_HOME` 优先级，不需要真实 JDK。

## InstallerTests.py

```bash
python3 src/tests/server/InstallerTests.py --script src/server/scripts/zt-installer.sh
```

在沙箱路径与假系统命令下执行独立安装脚本，验证 Moon 导出、ztncui 安装/卸载、ZeroTier 身份保留/删除顺序。不会实际安装或卸载系统组件。
