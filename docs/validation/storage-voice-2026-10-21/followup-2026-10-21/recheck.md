# A01–A07 修复复核（2026-10-21）

结论：**修复有实效，但不能全部关闭。A02、A03、A06、A07 已核实修复；A01、A04、A05 仍各有一个已复现的 P1 缺口。** 本记录追加到同日修复记录之后，不改写原有测试证据。根 GOAL.md 中“A01–A07 已关闭”的结论目前仍不成立。

范围：阅读本目录 README、对照上一轮审计、检查当前源码、增量重建 `storage_tests`／`core_tests`／`plugin_loader_tests`，复跑 8 个针对性功能用例，并用隔离探针补查失败与并发时序。未修改功能代码、版本、发行物或任务状态，也未执行阶段文件清理。

## 逐项结果

| 原发现 | 本次判断 | 核实结果 |
| --- | --- | --- |
| A01 失败迁移被新配置占据 | 部分修复 | 原插件配置复现场景已受保护；宿主 `settings.ini` 仍没有相同保护，见下文。 |
| A02 显示名越界迁移 | 已修复 | 候选目录名与实际父目录均有校验；对应专项重新运行通过。 |
| A03 生产待删记录无法恢复 | 已修复 | `dataRoot` 已正确；正常删除留下的真实记录可以恢复，专项与隔离探针均通过。 |
| A04 恢复绕过锁／安装引用 | 部分修复 | 包锁、重装引用、包哈希场景已修复；数据协调锁未覆盖“检查租约 → 移包”整体，仍有可复现竞态。 |
| A05 同名不同 ID 认领旧数据 | 部分修复 | 两份正常所有权记录时已修复；其中一份所有权记录损坏时仍会误认领，真实包扫描路径可复现。 |
| A06 发行物过期 | 已修复 | SDK 来源 44/44、SDK 文件 48/48 匹配；官方包哈希与 README 一致；主程序 ZIP 与目录的 191 个文件逐一匹配。 |
| A07 迁移复制插件日志／缓存 | 已修复 | 顶层 `cache/`、`logs/` 按迁移类别排除；对应专项重新运行通过。 |

## A01 剩余：宿主设置迁移失败后仍可写入（P1）

位置：`src/client/main/main.cpp:58`、`src/client/main/storage/SettingsStore.cpp:124`、`src/client/main/storage/StorageContext.cpp:451`、`:648`。

新增保护仅用于 `HostServicesImpl` 的插件文件写入。`migrationComplete()`／`migrationCorrupt()` 的状态没有接入宿主 `SettingsStore` 或两处 QML Settings。主程序在个人根迁移失败后，仍用目标 `settings.ini` 初始化设置；普通 setter 和同步照常成功。

隔离复现：

1. 共享 `settings.ini` 包含 `appearance/theme=dark` 和 `names/networks/test=Original Name`。
2. 用独占文件句柄暂时阻止读取来源，选择个人根并 `prepare()`；返回成功，`migrationComplete=false`。
3. 通过真实 `SettingsStore` 保存 `appearance/theme=system`，同步成功，目标文件被创建。
4. 释放来源句柄，再次启动迁移；原文件因目标已存在而被跳过，迁移转为完成，最终主题为 `system`，原网络备注没有恢复。

证据：[recheck-storage-probe.json](recheck-storage-probe.json) 的 `hostSettingsStillWritableDuringFailedMigration`：`normalSettingsWriteSucceeded=true`、`retryMigrationComplete=true`、`originalNameRecovered=false`。

建议：按 `settings` 迁移项保护整个宿主 INI 的 C++／QML 写入；不可仅将全局完成状态暴露出来。补充“宿主设置复制失败 → 正常编辑／QML 持久化 → 重启重试”的用例。

## A04 剩余：新租约可在等待协调锁期间登记，原包仍被移走（P1）

位置：`src/client/main/plugins/PluginLoader.cpp:660`、`:673`、`:678`。

恢复已持有安装目录包锁，但 `hasOtherLeases()` 的首次检查不在数据根协调锁内；随后 `completeLegacyImport()` 单独获取并释放协调锁，恢复再执行 `rename()`。加载方登记租约不依赖包锁，因此包锁不能封住这个间隙。

确定性隔离复现：

1. 准备 `recorded` 阶段的真实待删包、业务数据和合法记录。
2. 注册方先持有数据协调锁，此时尚未写入自己的租约。
3. 恢复开始：无锁检查读到“没有其他租约”，随后在 `completeLegacyImport()` 等待协调锁。
4. 注册方在所持协调锁内写入租约，再释放锁。
5. 恢复获得锁、完成墓碑写入，随后仍移走原包；到删除数据前再次检查时，才发现租约并保留待删事务。

结果：`leaseCreated=true`、`originalStillInstalled=false`、`packageMovedToPending=true`、`recordStage=moved`。数据本次受到保护，没有删除；但活租约对应的安装包已被移走，违反租约检查与目标隔离必须处于同一协调事务的要求。

证据：[recheck-loader-probe.json](recheck-loader-probe.json) 的 `leaseRegisteredWhileRecoveryWaitsForCoordination`。该时序重复两次均复现，使用临时目录和协作锁，未修改生产实现或注入宿主回调。

建议：按“包锁 → 数据协调锁”顺序持锁，在同一协调锁区间内重新校验所有权、检查租约、写墓碑／记录并移包。向支持 `held` 参数的存储方法传入已持有的锁，避免再嵌套获取。新增上述等待锁期间出现租约的回归；只在调用恢复前预写租约无法覆盖此时序。

## A05 剩余：归属登记失败只告警，仍允许自动迁移（P1）

位置：`src/client/main/storage/PluginDataStore.cpp:186`、`:231`；`src/client/main/plugins/PluginLoader.cpp:288`。

`registerInstalledNames()` 遇到其他 ID 的损坏所有权记录会跳过并返回告警；加载器只打印告警，仍继续。之后 `nameClaimedByOther()` 又跳过不可解析记录，导致已知的同名安装冲突丢失。保留损坏文件本身，并不等于保留了它应保护的旧数据目录。

隔离复现：

1. 安装两份真实、认证成功的 `Shared Name` 测试包，ID 分别为 `test-twin-a`／`test-twin-b`。
2. 准备旧目录 `plugins-data/Shared Name/keep.json`，将 `test-twin-b.json` 所有权记录设为损坏 JSON。
3. 执行真实 `PluginLoader::scan()`，扫描完成，两份包均认证成功。
4. 正常构造 `test-twin-a` 的 `HostServicesImpl`。初始化成功，旧目录被搬入第一个 ID，无法继续保留供人工确认。

证据：[recheck-loader-probe.json](recheck-loader-probe.json) 的 `authenticatedTwinScanWithCorruptOwnership`：`authenticatedPackages=2`、`consumerReady=true`、`legacyPreserved=false`、`firstIdClaimedLegacy=true`。直接调用名称登记的告警与损坏记录保留结果另见 [recheck-storage-probe.json](recheck-storage-probe.json) 的 `sameNameWithCorruptOwnership`。

建议：完整认证安装清单中的名称冲突应在内存中保留并参与迁移决策；元信息登记失败、不可读或锁竞争时，不得继续进行无法确认归属的自动迁移。补充“同名包 + 另一 ID 元信息损坏／不可写”的测试。

## 验证与边界

本轮重建后实际运行：

| 目标 | 功能用例 | QtTest 汇总与原始输出 |
| --- | --- | --- |
| `storage_tests` | 迁移写入保护、旧显示名边界、排除插件日志／缓存（3 项） | 5 passed，0 failed；[输出](recheck-storage-output.txt) |
| `core_tests` | 失败迁移阻止真实插件消费者写入（1 项） | 3 passed，0 failed；[输出](recheck-core-output.txt) |
| `plugin_loader_tests` | 生产记录往返、恢复包锁／重装引用、同名双包、各阶段恢复（4 项） | 6 passed，0 failed；[输出](recheck-loader-output.txt) |

QtTest 汇总包含各目标的 `initTestCase`／`cleanupTestCase`。合计 8 个功能用例通过；新增探针退出码也为 0，表示成功完成上述缺口复现，不表示目标行为正确。

另核对了原修复记录中的 CTest 10/10、存储 24、核心 27、加载 28、媒体引擎 19 和媒体集成通过的原始输出；这些属于先前修复轮次，本轮没有重复执行全部套件或真实音频测试。通过的已有用例不覆盖本次追加的三条时序／失败场景。

发行物独立检查：[recheck-artifact-content.json](recheck-artifact-content.json)。主程序 ZIP 的目录项先统一分隔符后排除，只比较 191 个文件；SDK 文件校验 48/48。SDK 来源 44/44 及主程序／SDK ZIP／媒体包／语音包的 SHA-256 均与本目录原 README 的记录相符，媒体／语音发行包也与其中间包一致。本轮没有重新打包它们。

原记录明确标注的隔离 GUI 交互启动、macOS、Linux 客户端原生只读、真实多用户 ACL、多人听感等未验证项继续保留；本次未把它们计为已通过，也未将其新增为 A01–A07 的修复失败。

建议先补齐 A01、A04、A05 的三个剩余缺口，再更新 GOAL.md 的完成状态并决定阶段清理。A02、A03、A06、A07 无须因本次复核重复实现。
