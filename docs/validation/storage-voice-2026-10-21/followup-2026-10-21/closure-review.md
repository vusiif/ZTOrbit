# 上一轮三项缺口的关闭复核（2026-10-21）

结论：**上一轮剩余的 A01、A04、A05 均已确认修复，可关闭这三项。** 当前源码、针对性回归及原复现探针的结果一致，本次未发现阻断这三项关闭的问题。验证平台为 Windows 11 x64、MSVC 2022、Qt 6.8.3、Release。

本记录在 [remaining-fixes.md](remaining-fixes.md) 之后追加；此前 [recheck.md](recheck.md) 描述的是修复前状态，其原始证据保留。本轮仅复核、增量构建测试和保存证据，没有修改功能代码、版本或发行物，也未执行阶段文件清理。

## 三项结果

| 发现 | 代码核对 | 本轮实测 |
| --- | --- | --- |
| A01 宿主设置迁移失败后被新配置占据 | `main` 和 `AppController` 均在读取／迁移旧设置前调用 `guardStorageMigration()`；受保护的 `SettingsStore` 拒绝 setter、remove、sync、旧来源导入，析构不主动同步。两处 QML `Settings` 已移除，窗口、侧栏和列宽经同一后端写入。 | 共享 `settings.ini` 读取失败时，普通写入失败且目标不创建；重启重试恢复原 `dark` 主题和 `Original Name` 备注。真实 QML 窗口加载、改尺寸／列宽并销毁，同样没有创建待恢复的 INI；恢复后正常持久化通过。 |
| A04 等待协调锁期间出现新租约，仍先移走原包 | 恢复先持有包变更锁，再取得数据协调锁；在同一持有区间内检查租约、写墓碑、移包及删数据，将 `held` 锁传给存储方法。 | 复用上一轮“注册方先持锁、随后写租约再释放”的时序：原包仍在安装目录，待删区没有该包，业务数据保留，记录停留在 `recorded`。新增跨进程回归还验证移除租约后，同一记录可以恢复完成。 |
| A05 另一同名 ID 的归属记录损坏，先初始化者错误认领旧目录 | 扫描保存并传递完整 `InstalledNameClaims`；登记失败、无法认证或归属不可读时保留目录，名称冲突亦在内存中参与判断。 | 两个真实认证包同名，其中一个所有权 JSON 损坏：扫描完成，正常消费者可以初始化，但旧目录仍在原处，未搬入第一个 ID；损坏记录保留。不能认证的包及无内存清单时的不确定归属也有回归覆盖。 |

原复现探针按当前生产调用方式接入 `guardStorageMigration()` 和扫描返回的 `InstalledNameClaims`；来源文件独占句柄、损坏所有权记录、协调锁等待及新租约登记等故障条件保持不变，未修改生产实现来干预结果。

独立探针证据：

- [closure-storage-probe.json](closure-storage-probe.json)：`normalSettingsWriteSucceeded=false`、`protectionApplied=true`、`originalNameRecovered=true`、`finalTheme=dark`；同名旧目录保留且未被第一个 ID 认领。
- [closure-loader-probe.json](closure-loader-probe.json)：`originalStillInstalled=true`、`packageMovedToPending=false`、`dataPreserved=true`、`recordStage=recorded`；真实双包扫描 `authenticatedPackages=2`、`legacyPreserved=true`、`firstIdClaimedLegacy=false`。原生产删除记录往返与包锁占用保护也保持正确。

## 本轮实际执行

增量重建：

```powershell
cmake --build build/cmake-build-plugin-tests --config Release --target `
    storage_tests core_tests plugin_loader_tests zerotier_gui_tests --parallel 4
```

重建成功后运行了以下 9 个功能用例，全部通过，无失败或跳过：

| 目标 | 功能用例 | QtTest 汇总 |
| --- | --- | --- |
| `storage_tests` | `settingsMigrationGateBlocksHostIniUntilRetry`、`legacyMigrationWaitsForConfirmableNameOwnership` | 4 passed；[输出](closure-storage-output.txt) |
| `core_tests` | `pendingMigrationBlocksConsumerInitializationUntilRetry` | 3 passed；[输出](closure-core-output.txt) |
| `plugin_loader_tests` | `recoveryKeepsPackageWhenLeaseAppearsUnderTheLock`、`sameNameScanWithCorruptOwnershipKeepsLegacyData`、`unauthenticatedPackageKeepsLegacyDisplayNameData` | 5 passed；[输出](closure-loader-output.txt) |
| `zerotier_gui_tests` | `failedHostSettingsMigrationBlocksQmlPersistence`、`manualColumnsPersistAndDetailsRemainNonModal`、`gpuAccelerationPreferenceSurvivesWindowRecreation` | 5 passed；[输出](closure-gui-output.txt) |

QtTest 汇总包含各目标的初始化与清理；功能用例数为 2+1+3+3=9。GUI 使用 offscreen 软件绘制，Qt 插件及 QML 导入路径指向受控 Qt SDK。独立探针使用 `build/` 下的临时树，不读写真实用户配置。

GUI 输出保留了无托盘图标的警告，以及测试结束时一次临时目录清理告警。进程退出后核对该测试目录只剩空缓存目录，已逐级清除；未出现断言失败，也未更改测试来隐藏警告。

## 发行物一致性

独立检查结果见 [closure-artifacts.json](closure-artifacts.json)：

- 宿主 SDK 来源指纹 44/44 一致，安装内容 48/48 一致。
- 主程序目录与 ZIP 均含 187 个文件，路径及逐文件 SHA-256 全部一致。
- 主程序 EXE、ZIP、客户端 SDK ZIP、媒体包和语音包的 SHA-256 均与 `remaining-fixes.md` 一致；本轮没有重新发布这些文件。

本轮未重复执行先前记录的整套 14 个 CTest，也未新增 macOS、Linux 原生只读、真实多用户 ACL 或多人听感验证；这些边界继续沿用原记录。这里的关闭结论针对上一轮三个已明确的缺口及 Windows x64 复核结果。
