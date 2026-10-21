# 存储修复跟进验证（2026-10-18）

范围：按 [阶段归档](../README.md) 先完成 **T01–T04**（个人根迁移重试、包变更锁、待删事务恢复、日志/缓存自动调度），并在同日追加完成 **T05–T10**（删除所有权、旧目录租约、设置迁移锁、语音旧键清除、clip 终态与尾音、存储扫描后台化）。T11–T18 尚未完成，本记录不代表 `GOAL.md` 已整体验收。

> 2026-10-18 追加：**T11–T14** 已完成，证据见文末“T11–T14”章节；T15–T18 仍未完成，本记录仍不代表 `GOAL.md` 已整体验收。

环境：Windows 11 x64，MSVC 2022，Qt 6.8.3（`build/cmake-build-tools/Qt/6.8.3/msvc2022_64`），Release；客户端 `0.5.1-alpha`／API `0.5-alpha`，未修改 `VERSION`、`SERVER_VERSION` 或插件版本。

## 构建

```powershell
$QtRoot = "build/cmake-build-tools/Qt/6.8.3/msvc2022_64"
cmake --build build/cmake-build-plugin-tests --config Release --target storage_tests plugin_loader_tests core_tests zerotier_gui_tests
```

结果：四个目标均构建成功；`package_lock_probe` 随 `plugin_loader_tests` 一起生成。

## 自动化结果

```powershell
$env:PATH = "$QtRoot/bin;$env:PATH"
$env:QT_PLUGIN_PATH = "$QtRoot/plugins"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$QtRoot/plugins/platforms"
$env:QML_IMPORT_PATH = "$QtRoot/qml"
$env:QML2_IMPORT_PATH = "$QtRoot/qml"
ctest --test-dir build/cmake-build-plugin-tests -C Release -R "storage_core|dynamic_plugins|core_regressions|gui_regressions" --output-on-failure
```

| 测试 | 结果 |
| --- | --- |
| `storage_core`（`storage_tests`） | 通过；18 passed、0 failed（原 13 项） |
| `dynamic_plugins`（`plugin_loader_tests`） | 通过；20 passed、0 failed（原 18 项） |
| `core_regressions`（`core_tests`） | 通过；24 passed、0 failed（原 22 项） |
| `gui_regressions`（`zerotier_gui_tests -platform offscreen`） | 通过；31 passed、3 skipped（原生托盘等平台用例跳过） |

CTest 汇总：`100% tests passed, 0 tests failed out of 4`（总耗时 129.02 s）。

产物 SHA-256：

```text
tests/Release/storage_tests.exe       E27928B507112749BEB4B322FC572A28BC108C0D1EDF919F23E910E5DB2668C4
Release/plugin_loader_tests.exe       3B9C8948A8B152116C44B797A1E94EA0D1C7C5A3DDD67870289587B4F29CDB67
Release/core_tests.exe                5A10CBEC66A685F17F939D125BAA60896E6A6FD1DEBA7368B8B5FDF491893188
Release/zerotier_gui_tests.exe        E874542D47A041D9650D5B5C4719F9B5AADC79AFC5788BADAC1CC2D219377F3F
```

## T01 个人根迁移重试

实现：`StorageContext::prepare()` 改为读取 `.ztgui/migration.json` 的逐项状态，不再以 `profile.json` 是否存在决定是否迁移；项目状态为 `copied`／`existing`／`missing` 才视为完成，`failed`／`partial` 下次启动重试。新增复制 `.ztgui/settings-migration.json`；仍不复制 `leases/`、`pending-deletions/`、日志和缓存。目标已存在的文件/目录不覆盖，来源缺失记为 `missing`，损坏的迁移记录原样保留并提示，`route.json` 只记录选根。

测试：
- `StorageTests::personalMigrationRetriesSettledItemsAndCopiesOwnership`：用一个普通文件阻塞目标目录制造部分失败；首次 `completed=false` 且告警包含“下次启动会继续尝试”，配置未被默认值覆盖；移除阻塞后第二次启动完成，`config.json`、所有权、`settings-migration.json` 到位；第三次启动不再复制，用户删除的目标文件不会被重新填充。
- `StorageTests::corruptMigrationRecordIsPreserved`：损坏的 `migration.json` 字节保持不变，无迁移发生，告警可见。
- `StorageTests::personalMigrationMarksMissingSources`：来源不存在时对应项记录为 `missing`，迁移可完成且不创建默认设置文件。

未验证：真实崩溃发生在复制中途（测试使用可控失败注入）；真实只读 ACL 下的个人根回退沿用既有测试方法，未做新的原生 ACL 检查。

## T02 安装目录级包变更锁

实现：新增 `plugins/PackageChangeLock.h`，导入与删除共用插件目录 `.import.lock`（短超时、失败返回 `Busy`）；删除在取锁后重新校验普通文件、目录边界、认证元信息和剩余安装引用。需要数据根协调锁时固定先包锁、后 `.ztgui/coord.lock`。

测试：
- `LoaderTests::deleteAndImportUseTheSamePackageChangeLock`：helper 进程 `package_lock_probe` 持有 `.import.lock`；删除与导入均立即失败（删除耗时 < 1.5 s），包、数据和启用状态不变；helper 退出后删除成功。
- 既有 `deleteRequiresUnloadAndRemovesPackageDataAndCatalog` 覆盖正常删除与卸载前置条件。

未验证：Windows 文件占用导致锁文件短暂无法删除的极端时序（沿用既有构建/测试边界）。

## T03 待删事务先落记录、启动恢复

实现：确认删除先原子写入 `.ztgui/pending-deletions/<id>.json`（`format`、`id`、活动根、用户键、原包路径、待删路径、`stage`、`referenced`、时间戳），再关闭旧配置导入、移包、删数据、删待删包，最后移除记录；记录落盘失败不产生任何文件变更。启动恢复入口 `PluginLoader::recoverPendingDeletions()` 在 `AppController` 设置存储后、插件扫描前执行，只处理当前活动根记录，校验普通文件/目录、受控路径、同卷、租约与包认证；不明确时保留记录并显示。设置页“存储与清理”新增待完成事务和自动维护结果提示。

测试：
- `LoaderTests::pendingDeletionsRecoverEveryStage` 覆盖：记录后崩溃（包未移动）、移包后崩溃（数据未删）、数据已删包未删、活租约阻止、越界待删路径拒绝、另一活动根记录不动、原包与待删包同时存在时保留；活租约释放后可继续完成。
- 正常删除路径（既有 `deleteRequiresUnloadAndRemovesPackageDataAndCatalog`）确认成功后记录被清理。

未验证：真实进程在 `QFile::rename` 与记录更新之间的强制终止；测试通过手工构造各阶段记录模拟，未做系统级崩溃注入。未认证包的“仅删包”入口属于 T05，尚未实现。

## T04 日志与业务缓存自动调度

实现：新增 `StorageMaintenanceScheduler`，复用 `StorageMaintenance::cleanupLogs()`／`cleanupCaches()`；启动后延迟一次、每 24 小时、日志轮转后和 `HostServicesImpl` 成功写入 `cache/` 后去抖触发；同一时间只运行一个任务，清理在 `QtConcurrent` 工作线程执行。`AppLog` 增加旋转回调，在释放日志互斥锁后调用。`AppController` 连接结果到“存储与清理”显示区，失败写宿主日志；自动任务不处理无主数据。同时修正日志/缓存总量淘汰在重新枚举后未按修改时间排序的问题，确保优先删除最旧文件。

测试：
- `core_tests::logRotationNotifiesCallback` 通过 `app_log_probe --rotation` 实际写满 10 MiB，断言回调恰好一次、生成两个日志文件。
- `core_tests::cacheWritesNotifyTheMaintenanceHook`：`HostServicesImpl` 只在成功写入/事务提交 `cache/` 路径时通知钩子；普通配置写入和 `Keep` 事务不通知。
- `StorageTests::maintenanceSchedulerDebouncesStartupDailyRotationAndCacheWrites`：注入清理任务与短间隔，断言启动、每日、重复轮转通知、重复 `cache/` 写入分别触发正确类别，`plugins-data/...` 写入不触发缓存清理，停止后不再运行。
- `StorageTests::retentionEnforcesByteLimits`：两个 60 MiB 日志触发 100 MiB 归属总量淘汰，两个 130 MiB 缓存触发 256 MiB 上限淘汰，均删除最旧文件。
- 既有 `logAndPluginCacheRetentionKeepsActiveAndRecentFiles` 继续覆盖活动日志不删、7/30 天规则。

未验证：真实 GUI 上长时间运行 24 小时定时器（仅用可注入短间隔验证同一路径）；自动清理与真实插件并发读取缓存的压力场景属于 T14，当前只保证文件写入/事务提交在协调锁内与清理互斥。GUI 回归构建（`ZEROTIER_TESTING`）不启动自动维护调度器，避免后台扫描干扰既有计时敏感的用例；生产构建启动调度器，调度器本身由 `storage_tests` 的注入用例覆盖。

## T05 删除前校验数据所有权，未认证包只显式保留数据

实现：`PackageResult` 增加独立的 `authenticated` 结果（认证成功但 API/运行契约不兼容仍记为已认证），插件目录扫描把它写入 catalog。`PluginLoader::removeFile()` 默认 `Removal::Full`：认证失败或 ID/名称与 catalog 不一致时直接拒绝，不删除包、不删数据、不写墓碑，并提示使用显式的“仅删除包文件、保留数据”入口；`Removal::PackageOnly` 只删除安装包并保留数据、所有权记录和启用状态。完整删除在破坏性步骤前读取 `.ztgui/plugins/<id>.json`：记录不可读或 ID 不匹配时取消；记录缺失允许继续，但先写墓碑。相同 ID 仍有其他安装包时保留共享数据与启用状态，并在结果中说明。删除确认框通过 `PluginLoader::deletionPlan()` 在操作前显示是否会删除数据。

测试：
- `LoaderTests::tamperedPackageKeepsDataAndRequiresPackageOnlyRemoval`：加载后篡改密文并重新扫描，`authenticated=false`；完整删除失败且包、数据、所有权记录、待删记录均未变化；`PackageOnly` 只删包，数据仍在。
- `LoaderTests::ownershipRecordMismatchOrCorruptionCancelsFullDelete`：所有权记录 ID 不匹配和损坏 JSON 均取消完整删除；移除记录后删除成功，并确认墓碑 `legacyImport.completed=true`。

未验证：真实用户通过外部工具产生的损坏包只做了字节篡改模拟；没有对只读 ACL 下的删除入口做原生权限验证。

> 更正：上文 T04 小节末尾“未认证包的‘仅删包’入口属于 T05，尚未实现”在本轮已由上面的实现取代；旧句保留不改。

## T06 旧显示名目录迁移前检查其他进程租约

实现：`PluginDataStore::prepare()` 在协调锁内、写本进程租约之前检查该 ID 的其他租约；只有目标稳定 ID 目录不存在且存在可迁移的旧显示名目录时才检查。任何其他租约（包括无法读取或归属不明）都会阻止迁移并给出警告；被阻止时不预创建空目标目录，租约释放后下次加载可以重新迁移，不合并、覆盖或删除数据。

测试：`StorageTests::legacyDirectoryMigrationWaitsForOtherLeases` 写入另一个 run 的租约，`prepare()` 保留旧目录、目标未创建并产生“租约”警告；删除该租约后再次 `prepare()` 成功把旧目录改名为 `id-voice-chat`，文件内容保留。

未验证：真实两个进程同时加载的时序由租约文件和协调锁模拟，未启动第二个 host 进程。

## T07 共享根首次设置迁移使用协调锁

实现：`CoordinationLock` 已是独立头文件／实现，由 `PluginDataStore`、`HostServicesImpl`、`StorageMaintenance` 共用；本轮让 `SettingsStore::migrateLegacy()` 也复用它。读取完成标记、读取来源、写目标、删除来源键和写标记的全过程持有 `.ztgui/coord.lock`（2 秒超时）。锁被占用时返回未完成、保留全部来源键、不写 `completed=true`；释放后下一次迁移完成。锁顺序不与包变更锁反向嵌套。

测试：`StorageTests::settingsMigrationHoldsTheCoordinationLock` 在当前进程持有协调锁时迁移返回失败、来源键保留、无完成标记；释放后迁移成功并清除来源键。

未验证：跨进程锁竞争由 `QLockFile` 行为保证，同进程测试已能观察到锁失败；未另建 helper 进程。

## T08 语音旧配置源键在成功后清除

实现：`legacySettings()` 记录每个键的实际来源应用（当前 `ZTGUI`／历史 `ZeroTier GUI`）和规范化值（设备十六进制小写、音量 0–200、布尔、字符串）。目标通过 `updateFile` 提交并重新读取校验后才处理来源：删除本次实际补缺的键，以及目标值与旧值相等的冗余键；冲突、未知和未迁移键保留。每次删除后 `sync()` 并用新的 `QSettings` 重新读取确认；失败产生警告、保持导入资格开放以便下次重试，不写完成标记。目标提交失败时不删除任何来源键。

测试：
- `VoiceUiTests::legacyConfigClearsOnlyImportedSourceKeys`：当前／历史来源的全部旧键、二进制设备、超范围音量、冲突音量和未知 `port`；断言目标优先级、设备十六进制规范化、已迁入键删除、冲突与未知键保留、第二次加载幂等。
- `VoiceUiTests::legacySourcesSurviveTargetCommitFailure`：目标写入失败时来源键全部保留且未关闭导入资格。
- 既有 `devicePreferencesSurviveAsynchronousDiscovery` 增加来源键已清除的断言。

未验证：Windows 上让来源键删除失败需要只读设置文件或权限 ACL，测试没有注入该故障，只验证了“目标提交失败不删来源”。

## T09 `media.clip` 失败事件可达，尾音不截断

实现：新增 `ClipPlayer` 状态机，为每个 clip 保证恰好一个终态事件：分块被设备拒绝、设备消失／sink 停止或排空超时发出 `failed`；否则等待该轨混音队列和输出未写积压都为空后发出 `finished`。`DeviceManager` 新增只读 `playbackState(device, track)` 和“播放接受失败”检查；`SessionEngine` 的 `stopClip()` 改为静默取消，不再保留未使用的 `notifyFailure` 参数。`VoiceSoundPlayer` 对 `failed`（含空／未知 clip ID）立即推进队列，并用播放序号忽略失败后迟到的旧播放回执，避免卡在 `starting` 或错认新 clip。

测试：
- `media_engine_tests` 新增 6 项 `ClipPlayer` 用例：投喂失败只发一次 `failed` 且停止继续投喂、混音队列／积压清空后才 `finished`、输出设备消失失败、排空超时失败、设备回执丢失时整体超时失败、`stop` 静默。
- `MediaIntegrationTests::clipServiceReportsFailureAfterAcceptance`：通过测试专用注入（worker 环境变量指向测试文件，生产不设置）让第 3 块被拒绝，断言接受后发出一次 `failed`、无 `finished`，且同一租约随后仍能正常完成一个 clip。
- 既有 `clipServiceAcceptsAndFinishesShortAudio` 使用本轮重新构建的本地媒体包通过（真实输出设备）。
- `VoiceUiTests::soundQueueAdvancesOnFailedAndUnknownClipIds`：空 ID、未知 ID、已知 ID 失败都推进队列，迟到回执不重启 clip。

未验证：集成测试使用本地 `build/cmake-build-media-v2-worker/Release/ztgui_media_worker.ztplugin`，`build/dist/` 中的媒体包尚未按本轮源码重打包（属于 T12 的发行重建范围）；真实设备在播放中被物理拔出的时序由注入失败和状态查询覆盖，未做硬件拔插。

## T10 存储扫描后台化，展示待清理项

实现：`StorageMaintenance` 抽出与清理一致的日志／缓存保留计划，新增只读 `previewRetention()`。`AppController::refreshStorageUsage()` 改为 `QtConcurrent` + `QFutureWatcher`，后台同时计算占用、待清理项和无主数据；新增 `storageScanBusy`、`storageScanFinished`、`scanError` 和 `scannedAt`，结果缓存供 `storageOrphans()` 使用。扫描与清理互斥：扫描中发起的清理排队到扫描完成，清理完成后自动重新扫描。日志／缓存清理在确认框中展示待删除数量与字节数；无主数据列表区分可删除与需人工确认。`SettingsPage.qml` 显示待清理预览、无主数据摘要和扫描／清理进行中状态，按钮在两种忙碌状态下禁用。

测试：
- `QmlTests::storageSectionShowsPathsAndCleansOrphanData` 改为等待异步扫描完成，并断言预览字段存在。
- `QmlTests::storageScanIsAsynchronousAndPreviewsPendingCleanup`：构造 400 个文件，断言 `refreshStorageUsage()` 立即返回、扫描期间事件循环心跳、预览计数正确、日志／缓存清理各弹一次确认并实际删除；清理期间的扫描请求会排队。
- `StorageTests::logAndPluginCacheRetentionKeepsActiveAndRecentFiles` 增加预览计数断言（活动日志不计入），清理后预览为空。

未验证：真实网络盘或数十万文件的扫描耗时；测试通过异步信号和批量文件覆盖 GUI 不阻塞，而不是精确性能基准。

## T05–T10 回归结果

```powershell
$QtRoot = (Resolve-Path "build/cmake-build-tools/Qt/6.8.3/msvc2022_64").Path
$ff = (Resolve-Path "build/cmake-build-media-dependencies/installed/bin").Path
$env:PATH = "$ff;$QtRoot/bin;$env:PATH"
$env:QT_PLUGIN_PATH = "$QtRoot/plugins"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$QtRoot/plugins/platforms"
$env:QML_IMPORT_PATH = "$QtRoot/qml"
$env:QML2_IMPORT_PATH = "$QtRoot/qml"
$env:QT_QUICK_BACKEND = "software"
ctest --test-dir build/cmake-build-plugin-tests -C Release -R "storage_core|dynamic_plugins|core_regressions|gui_regressions"
ctest --test-dir build/cmake-build-media-v2-worker -C Release -R "media_engine"
```

| 测试 | 结果 |
| --- | --- |
| `storage_core` | 通过；20 passed（T01–T04 的 18 项 + T06/T07 各 1 项 + 预览断言；T07 同进程锁用例通过） |
| `dynamic_plugins` | 通过；22 passed（T05 新增 2 项） |
| `core_regressions` | 通过；24 passed |
| `gui_regressions` | 通过；32 passed、3 skipped（新增 T10 扫描／清理用例；原生平台用例跳过） |
| `media_engine` | 通过；19 passed（新增 6 项 `ClipPlayer` 用例） |
| `voice_ui_tests`（手动） | 通过；19 passed、0 skipped（新增 T08 两项、T09 队列失败用例） |
| `media_integration_tests`（手动，本地媒体包覆盖） | `clipServiceAcceptsAndFinishesShortAudio`、`clipServiceReportsFailureAfterAcceptance` 均通过；总计 4 passed |

CTest 汇总：宿主 4/4、媒体 1/1 通过。媒体集成测试使用：

> 稳定性说明：两次紧跟重新构建的完整 `ctest` 中，既有 `core_regressions::pluginStorageUpdatesAreAtomicAcrossProcesses` 出现过一次子进程非零退出（stderr 为空）；紧接的单独重跑与不再并发的完整 4/4 均通过，单独压力执行该探针 30 轮也全部为 0。该失败与构建后杀毒／磁盘负载的时序一致，本轮未修改该测试或跨进程更新协议，记录在案以免把偶发失败当作已修复。

```powershell
$env:ZTGUI_TEST_MEDIA_PACKAGE = (Resolve-Path "build/cmake-build-media-v2-worker/Release/ztgui_media_worker.ztplugin").Path -replace '\\','/'
build/cmake-build-plugin-tests/tests/Release/media_integration_tests.exe -platform windows clipServiceAcceptsAndFinishesShortAudio clipServiceReportsFailureAfterAcceptance
```

产物 SHA-256（2026-10-18 本轮重建）：

```text
tests/Release/storage_tests.exe                 7D4F9358EF782B4637DB794E51A009DE232712B0971EBC794447ECE67255BA7E
Release/plugin_loader_tests.exe                 6DD3867B7902E3FD46D5BB8322E7FB79F2033FA99F9486C3D28FCEFFA59D88AE
Release/core_tests.exe                          773ED98576395677A3A4A9D0AB847F472EAD2D17DFD0485351996B535B4265F6
Release/zerotier_gui_tests.exe                  1753F80CAFC2A1C2651BEFEC0C589D9F7A8CEEB955A510AE2C503A4EE0105D78
tests/Release/media_engine_tests.exe            FDABD4A2EE5389712B9C9F20C5A08BE45B52365841B5D9BCC053EA3CFF7E8F78
Release/voice_ui_tests.exe                      189A8021E48013EBC2E72A289BDF2F859011E37187CBA85FF5D69593900C1737
tests/Release/media_integration_tests.exe       BF0E897DAA4222943DBF3EF4364529BD26003F5865B2D6513917439817DD8A5C
Release/ztgui_media_worker.ztplugin             6D226B8AE73FC10A2F8BB2B9083C081EC3ED752EDA7330E09EDFBE65888979BD
```

## 与旧验证记录的关系

`docs/validation/storage-voice-2026-10-21/implementation-2026-10-18/README.md` 中“plugin_loader_tests 覆盖租约阻止与待删包”的表述早于本记录；本轮新增的跨进程包锁和六类待删恢复用例才提供对应证据。旧记录原文保留不改，待 T18 收口时统一核对。

## T11–T14（2026-10-18 追加）

### 构建与回归命令

```powershell
$QtRoot = (Resolve-Path "build/cmake-build-tools/Qt/6.8.3/msvc2022_64").Path
cmake --build build/cmake-build-plugin-tests --config Release --target `
    plugin_package_tests storage_tests plugin_loader_tests core_tests `
    plugin_services_tests zerotier_gui_tests voice_plugin_tests media_integration_tests

$env:PATH = "$QtRoot/bin;$env:PATH"
$env:QT_PLUGIN_PATH = "$QtRoot/plugins"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$QtRoot/plugins/platforms"
$env:QML_IMPORT_PATH = "$QtRoot/qml"
$env:QML2_IMPORT_PATH = "$QtRoot/qml"
$env:QT_QUICK_BACKEND = "software"
ctest --test-dir build/cmake-build-plugin-tests -C Release -R `
    "plugin_package_container|storage_core|dynamic_plugins|core_regressions|gui_regressions|plugin_services" `
    --output-on-failure
```

结果：`100% tests passed, 0 tests failed out of 6`。直接运行计数：

| 测试 | 结果 |
| --- | --- |
| `plugin_package_tests` | 11 passed、0 failed |
| `storage_tests` | 20 passed、0 failed |
| `plugin_loader_tests` | 23 passed、0 failed（原 22，新增 1） |
| `core_tests`（在 CTest 工作目录运行） | 25 passed、0 failed（原 24，新增 1） |
| `zerotier_gui_tests -platform offscreen` | 32 passed、3 skipped、0 failed |
| `plugin_services_tests -platform windows` | 25 passed、0 failed（新增 1） |
| `voice_plugin_tests -platform windows` | 7 passed、2 skipped（`ZTGUI_PREVIEW_PLUGIN` 已设置；跳过项为真实服务器用例） |

稳定性说明：一次紧跟完整重编的 `ctest` 中 `gui_regressions` 失败；该测试单独重跑以及随后两次完整 6/6 均通过，未修改该测试，也未保留当次失败输出。本轮按偶发时序失败记录，不视为已修复。

### T11 缓存清理 helper 的身份、索引、PID 与 worker 安全

实现：`CacheContext` 新增所有权令牌，`prepareCacheSession` 在索引登记失败时返回错误并拒绝继续解压；`owner.json` 记录宿主进程创建标识（Windows `GetProcessTimes`、Linux `/proc/<pid>/stat`、macOS `sysctl`），存活判断同时核对 PID 与创建标识，PID 复用按已退出处理。会话根新增 `workers.json` 并在准备会话时创建空记录，`ServiceBroker` 在 worker 启动时登记 PID／创建标识、退出/析构时移除。隐藏清理入口只接收 `--cache-user`／`--cache-run`／`--cache-token`，从用户缓存索引解析根路径，索引未登记、令牌不匹配、结构越界、目录含链接、残留 worker 仍存活或缺少 worker 记录的旧会话都跳过并保留索引；`cleanStaleCache` 复用同一校验。修复了索引更新时读句柄未关闭导致 `QSaveFile` 在 Windows 上“拒绝访问”的问题。

测试：
- `PluginPackageTests::cacheSessionOwnershipAndCrashRecovery`：索引登记与令牌、未登记根拒绝、令牌不匹配、PID 复用（当前 PID + 伪造创建标识）不误保留、活动 worker 阻止回收且注销后可回收、缺少 worker 记录的旧会话保守保留、索引写入失败时 prepare 失败、目录结构越界拒绝。
- `PluginServicesTests::workerRegistrationTracksBrokerLifetime`：在受管缓存会话中启动真实 worker，断言 `workers.json` 记录 broker PID 与创建标识，正常停止后记录清空。
- 既有 `hostDeathTerminatesOwnedTree` 继续通过。

未验证：真实进程在索引写入与解压之间被强杀；真实操作系统层面的 PID 复用（测试用当前 PID 配合错误创建标识模拟）；非 Windows 平台宿主崩溃后残留 worker 的实际回收；目录含链接的清理拒绝没有单独夹具。

### T12 重建并运行独立宿主/媒体 SDK 消费者

实现：`src/tests/sdk-consumer/ApiConsumer.cpp` 包含 `<ztgui/PluginStorage.h>`，对 `PluginStorage` 做编译期抽象类检查并在函数中 `dynamic_cast` 后调用 `updateFile`／`legacyImportAllowed`／`completeLegacyImport`。在独立构建目录使用发布 SDK 重新配置、构建并运行宿主消费者；媒体预览消费者从 `build/dist/ZTGUI-media-api-v2/examples/preview` 构建，不引用仓库源码。`VoicePluginTests` 新增 `oldMediaPackageWithoutClipServiceIsRejected`：从当前媒体包解包后重打包一份去掉 `media.clip` 的提供声明，断言语音加载被依赖计划以 `media.clip` 拒绝。`previewSdkConsumerRendersVideo` 调整到测试类最后执行，因为真实 Qt Quick Controls 页面会把媒体包内的 `Qt6Svg` 映射进宿主，若先运行会污染 `repeatedLoadsLeaveNoMediaModulesOrWorker` 的宿主模块隔离检查。

命令与结果（cwd 为仓库外 `%TEMP%\ztgui-t12-cwd`，PATH 只加入发布 API SDK 与 Qt）：

```powershell
cmake --build build/cmake-build-sdk-consumer --config Release
# sdk_consumer.exe -> exit 0

cmake -S build/dist/ZTGUI-media-api-v2/examples/preview -B build/cmake-build-media-preview-sdk `
    -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_PREFIX_PATH=build/dist/ZTGUI-api-v0.5-alpha;build/dist/ZTGUI-media-api-v2;$QtRoot"
cmake --build build/cmake-build-media-preview-sdk --config Release
# ztmedia_preview_probe.ztplugin 由发布 SDK 的 ztgui_package_plugin() 生成

$env:ZTGUI_PREVIEW_PLUGIN = (Resolve-Path "build/cmake-build-media-preview-sdk/Release/ztmedia_preview_probe.ztplugin").Path
voice_plugin_tests.exe -platform windows
# 7 passed、2 skipped；previewSdkConsumerRendersVideo 通过，不再跳过
```

产物 SHA-256：

```text
build/cmake-build-sdk-consumer/Release/sdk_consumer.exe                              F1182526E8A583B17A531956689FF812D242BAFDE67878EE72C7A4BAF23AA5D5
build/cmake-build-media-preview-sdk/Release/ztmedia_preview_probe.ztplugin          F1BBDA57F12D2831B48A26D2625AD1551640101238BD2D12CED0A6A89B0D133E
```

未验证：`build/dist` 中的媒体／语音发行包未按本轮源码重新打包（仍为 2026-10-18 20:00 产物，晚于 T09 的本地 worker 构建），本轮只验证发行 SDK 构建出的消费者，不声称发行包与最新媒体源码一致；发布 SDK 目录也没有因宿主内部缓存实现变更而重新安装（公共头文件和 API 代际未变，消费者按现有发布目录构建）；媒体预览只验证合成画面回送，真实设备播放沿用既有条件跳过项。

### T13 无主数据对不可解析包标记“不确定”

实现：`PluginLoader::hasUncertainPackages()` 检查仍存在的 catalog 行，任何包没有可认证 ID 即返回真；`AppController` 把该状态传给后台存储扫描。`StorageMaintenance::orphans(installedIds, uncertainPackages)` 在不确定时把所有未被已安装 ID 使用的 `id-*` 目录设为 `deletable=false`，原因写为“存在无法解析或未通过认证的安装包，数据归属不确定，按人工处理保留”；设置页的“存储与清理”文本增加对应说明，确认列表仍只展示可安全删除项。旧显示名目录继续永远人工处理。

测试：
- `LoaderTests::unparsablePackageMarksOrphanDataUncertain`：安装目录放入无效 `.ztplugin` 与孤立 `id-ghost` 数据，断言 `hasUncertainPackages()` 为真、`orphans({}, true)` 不可删除且原因正确；删除损坏包后重新扫描，同一目录恢复可删除。
- `StorageTests::maintenanceSeparatesOrphansFromAmbiguousLegacyData` 增加 `orphans({"installed"}, true)` 断言。

未验证：真实第三方工具产生的损坏包只做了无效字节模拟；设置页没有新增独立控件，只在现有无主数据文本中说明。

### T14 文件读取与缓存清理协调

实现：`HostServicesImpl::readFile` 对 `cache/` 路径先在短时数据根协调锁内取得该文件的活动锁（`.ztgui/read-locks/<sha256>.lock`），释放协调锁后再读取，活动锁持有到读取结束；其他路径保持原进程内锁路径。`writeFile`／`updateFile` 的 cache 路径也取得同一活动锁，避免与读取或清理交叉。清理在协调锁内等待每个候选的活动锁最多 1 秒，取得后删除；读取中或外部占用导致无法取得/删除的记录到 `skipped`，真实删除失败记录到 `failed`；不强制删除。该机制只对 `cache/` 生效，大文件读取不会长时间持有全局协调锁。

测试：
- `CoreTests::cacheReadsCoordinateWithBackgroundCleanup`：写 512 KiB 闲置缓存文件，后台线程持续 `readFile("cache/data.bin")`，主线程触发 `cleanupCaches()`；断言读取未出现意外失败、清理报告成功释放、文件与空 cache 目录最终移除。
- 既有 `logAndPluginCacheRetentionKeepsActiveAndRecentFiles` 继续通过。

未验证：真正的第二个宿主进程并发读取（测试用同进程线程 + `QLockFile` 的跨进程文件锁语义）；由外部非协作进程持有句柄导致的“在用跳过”分支没有稳定夹具；超过 1 秒的慢读取会被清理按在用跳过保留到下一轮，未做超大缓存压力基准。

### T11–T14 产物 SHA-256

```text
tests/Release/plugin_package_tests.exe          2BE741236CBD2FAAD5BD660B91F6E164A790777D31404EA7B34ACDAC8FFF9D96
tests/Release/storage_tests.exe                 BFD2ED6BAFDFCA4F8CC6446713EE91E0AFC0233AFE35570B2EB346789F8161A1
Release/plugin_loader_tests.exe                 DE3BA37699326C06A8098F9D0A5DA3BDDCC9DBD2453A734E097EB0AFC289ED24
Release/core_tests.exe                          202DA22345C7A2E8DE958F11898F85CA3EC613261BB2EEF8AED7FEECA07B287D
Release/zerotier_gui_tests.exe                  94B162C0331C23B3059E1B40FF3591875A85EBC2ECC78E8053B8C9B64CD62371
Release/plugin_services_tests.exe               03BA9B2202A49DF9CDD615F1B7FF7BF85CC776E7D46CBF7C8613C63262A538C6
Release/voice_plugin_tests.exe                  2D6DDF17B21FCA1A8534572B1C7CE3F4DDA60EEAA34B2DC84FED3E2F3837FDD7
tests/Release/media_integration_tests.exe       2E4F671749470AF77A1FCA30476BB6E33E6ABDE9ECAD35471E60CC644C19AA08
```

`media_integration_tests` 本轮重新构建并运行 `clipServiceAcceptsAndFinishesShortAudio` 冒烟用例（3 passed、0 failed），未重跑全部真实设备用例；媒体包仍为既有发行包，T09 的失败/尾音验证仍以 `build/cmake-build-media-v2-worker/Release/ztgui_media_worker.ztplugin` 的专项记录为准。T15–T18 未完成。
