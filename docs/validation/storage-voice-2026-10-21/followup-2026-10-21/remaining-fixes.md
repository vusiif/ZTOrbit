# 复核剩余项修复（2026-10-21 第二轮）

结论：**再次复核留下的三个 P1 缺口（A01、A04、A05）已修复并有针对性回归；A02、A03、A06、A07 维持上一轮结论。** 本轮只改宿主私有实现、QML 与测试，公共 SDK、插件版本、媒体／语音包和服务端未变；主程序已重新构建并重打 Windows 发行目录与 ZIP。上一轮修复记录、[再次复核](recheck.md) 及其原始输出保持不变。

环境：Windows 11 x64，MSVC 2022，Qt 6.8.3（`build/cmake-build-tools/Qt/6.8.3/msvc2022_64`），Release，客户端 `0.5.1-alpha`／API `0.5-alpha`。未修改 `VERSION`、`SERVER_VERSION`、插件版本、媒体／语音包或服务端源码。

## 逐项修复

| 原发现 | 修复位置 | 回归 |
| --- | --- | --- |
| A01 宿主设置迁移失败后被新配置占据 | `StorageContext::migrateSharedData()` 按 `settings` 项设置 `settingsMigrationPending()`（记录损坏时为真）；`SettingsStore::protectFromWrites()`／`guardStorageMigration()` 拒绝全部持久写入且不创建目标文件，析构也不同步；`migrateLegacy()` 在受保护时提前返回、不写完成标记；`main` 与 `AppController` 都应用该门控。QML 窗口尺寸／侧栏／列宽改由 `backend.hostSetting()`／`setHostSetting()` 经同一 `SettingsStore` 读写，删除两处 QML `Settings` 元素。 | `StorageTests::settingsMigrationGateBlocksHostIniUntilRetry`；`QmlTests::failedHostSettingsMigrationBlocksQmlPersistence` |
| A04 恢复在等待协调锁期间可登记新租约 | `PluginLoader::recoverPendingDeletions()` 在包锁之后先取得数据根协调锁，再在同一持有区间内复查租约、复查所有权、写导入墓碑、移包和删数据，并把已持有的锁传给 `completeLegacyImport()`／`deleteDataDirectory()`；不再在锁外先调用 `hasOtherLeases()`。 | `LoaderTests::recoveryKeepsPackageWhenLeaseAppearsUnderTheLock`（新 `coordination_lock_probe` 协作进程） |
| A05 归属登记失败仍允许自动迁移 | 扫描把完整认证安装清单的显示名归属保留为 `PluginDataStore::InstalledNameClaims`（含 `issues` 与 `authoritative`），随宿主服务初始化传给每个消费者；`prepare()` 在另一已安装 ID 同名、登记失败、相关所有权记录不可读或安装目录内存在无法认证的包时保留旧目录并提示，不预创建目标。损坏记录与损坏包原样保留。 | `LoaderTests::sameNameScanWithCorruptOwnershipKeepsLegacyData`、`LoaderTests::unauthenticatedPackageKeepsLegacyDisplayNameData`；`StorageTests::legacyMigrationWaitsForConfirmableNameOwnership` |

### A01 补充：QML `Settings` 会自行创建目标文件

复核只测到 C++ `SettingsStore` 的写入，实际更严重：`Settings` 元素在加载时就把声明的默认属性写入文件。用 `qml.exe` 单独加载一个 `Settings { location: <临时 INI>; property int savedWidth: 1024; property real sidebarWidth: 190 }`，文件立即出现：

```text
[window]
qml\sidebarWidth=190
qml\savedWidth=1024
```

因此“仅在赋值处加布尔判断”仍会在迁移失败期间创建目标，让重试把该文件判为 `existing` 并跳过原 `settings.ini`。本轮把 QML 持久化并入 `SettingsStore`，宿主 INI 只有一个写入方，写入保护才真正覆盖 QML。键名与 category（`window/qml/*`、`tables/qml/<kind>/*`）保持不变。

## 新增回归要点

- A01：用 Windows 独占读句柄阻止共享 `settings.ini`，`prepare()` 为 partial 且 `settingsMigrationPending()` 为真；此时 C++ 普通保存返回失败、目标文件始终不存在、`settings-migration.json` 不写完成标记；QML 真实窗口创建、改尺寸、调整列宽并销毁后目标仍不存在。释放句柄再启动后迁移完成，恢复 `appearance/theme=dark` 与 `names/networks/test=Original Name`，随后 C++／QML 持久化恢复正常。
- A04：`coordination_lock_probe` 先持有 `.ztgui/coord.lock` 并写出标记文件，恢复开始后等待该锁；协作进程在仍持有锁时写入 `run-other` 租约再释放。恢复必须返回 0、问题说明包含“租约”、原包仍在安装目录、待删区为空、数据与记录保留；删除该租约后同一记录恢复成功。旧实现会在锁外读到“无租约”、随后仍移走原包，本用例会失败。
- A05：真实 `PluginLoader::scan()` 扫描两个同名、不同 ID 的认证包，其中 `test-twin-b` 的所有权记录为损坏 JSON。扫描返回 2 条内存归属、1 条登记问题；`test-twin-a` 的正常消费者保留旧目录且提示归属无法确认，`test-twin-b` 因自身记录损坏拒绝初始化，损坏记录逐字节保留。存储层另覆盖“仅内存归属即可阻止迁移”和“无清单时任一记录不可读即保守保留”；另用真实扫描验证安装目录内存在无法认证的包时不自动迁移，移除该包后同一目录才迁移。

## 自动化结果

```powershell
$QtRoot = (Resolve-Path "build/cmake-build-tools/Qt/6.8.3/msvc2022_64").Path
$env:PATH = "$QtRoot/bin;$env:PATH"
$env:QML_IMPORT_PATH = "$QtRoot/qml"; $env:QML2_IMPORT_PATH = "$QtRoot/qml"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$QtRoot/plugins/platforms"
$env:QT_QUICK_BACKEND = "software"
cmake -S . -B build/cmake-build-plugin-tests -G "Visual Studio 17 2022" -A x64 "-DCMAKE_PREFIX_PATH=$QtRoot"
cmake --build build/cmake-build-plugin-tests --config Release
ctest --test-dir build/cmake-build-plugin-tests -C Release
```

`QML_IMPORT_PATH`／`QT_QPA_PLATFORM_PLUGIN_PATH` 指向受控 Qt SDK，是因为该开发构建树只部署了 `windows` 平台插件和 Qt DLL、没有本地 `qml/` 目录；这是本机运行环境设置，不改被测代码，也不改变发行包的部署内容。

| 目标 | 结果 | 原始输出 |
| --- | --- | --- |
| `storage_tests` | 26 passed、0 failed（原 24，新增 2） | [输出](remaining-storage-output.txt) |
| `core_tests` | 27 passed、0 failed | [输出](remaining-core-output.txt) |
| `plugin_loader_tests` | 31 passed、0 failed（原 28，新增 3） | [输出](remaining-loader-output.txt) |
| `zerotier_gui_tests` | 33 passed、0 failed、3 skipped（本轮新增 1 项） | [输出](remaining-gui-output.txt) |
| CTest | 14/14 passed，413.27 秒 | [输出](remaining-ctest-output.txt) |

跳过项仍是既有平台边界：`windowsCaptionTrayAndRestore`（offscreen 平台）、`gpuAccelerationCommandLineOverrideIsTemporary`（由 `gpu_acceleration_override` CTest 单独运行）、`mediaOnlyUnloadMemory`（需 `ZTGUI_TEST_MEDIA_MEMORY=1` 的物理 worker 内存检查）；没有把跳过计为通过。

## 宿主发行物

按构建技能重建主程序（未加 `-RebuildApi`、未指定插件）：

```powershell
./.agents/skills/ztgui-build/scripts/build-windows.ps1 -QtRoot $QtRoot
# API inputs and installed SDK unchanged; reusing existing SDK
# Plugin compilation skipped (not requested)
```

- `build/dist/windows-x64/` 与 `windows-x64.zip` 均为 187 个文件，逐文件内容一致（187/187，0 差异）。
- 目录和 ZIP 不含 `plugins/`、FFmpeg／avcodec／Qt Multimedia 运行库、`.ztgui`／`plugins-data`／日志或设置文件；`ZTGUI.exe` 版本元数据 `0.5.1-alpha`。
- 文件数从 191 变为 187：`Main.qml`／`RecordList.qml` 不再 `import QtCore`，windeployqt 因此不再部署 `Qt6QmlCore.dll` 与 `qml/QtCore/`（`plugins.qmltypes`、`qmldir`、`qtqmlcoreplugin.dll`，共 4 个文件）。这是移除 QML `Settings` 的直接结果，不是内容缺失。
- 源码最新修改时间 `2026-10-21T18:51:13`（`PluginLoader.cpp`，发现任务中的归属确认）；`ZTGUI.exe` `19:03:33`、`windows-x64.zip` `19:03:42`，晚于所包含源码。构建脚本生成的 `windows-x64.previous-*` 备份已删除。

产物（SHA-256）：

| 产物 | 本轮值 | 说明 |
| --- | --- | --- |
| `build/dist/windows-x64/ZTGUI.exe` | `A657CFB0FCA6EB739632C47DAFFBEA65F53BAB4C4F06F3E11F35A5DF24CD81E1` | 重建 |
| `build/dist/windows-x64.zip` | `319A18C032FACCA2DD946A2C2D06C615687DF195B445A9D97318C5F742684A46` | 重打 |
| `build/dist/ZTGUI-api-v0.5-alpha.zip` | `C5515C0A3A843D5AEA4823EF3BF793A1BCF3AE2A872667B85587979E2B47BC71` | 复用（`src/client/api`、`src/shared/include`、`src/shared/package` 未变） |
| `build/dist/plugins/ztgui_media_worker-v2.0.0.ztplugin` | `A67EAAB57EC3769EE21A7584AFB7209B5B814CAAAEED5DB84A4AC21811B8B0AC` | 复用 |
| `build/dist/plugins/ztgui_voice_chat-v0.3.0.ztplugin` | `9D734EBD25A113995F7E1A37D4573CCD3D705CB4F365DEF2A3BB40EA11BC1057` | 复用 |

未重编媒体包、语音包与 Java 服务端：它们的源码、版本和依赖的公开 SDK 都没有变化；本轮改动仅在宿主私有实现、QML 和测试。宿主 `sdk-tools.json` 记录的共享包源码哈希也未变。

## 未验证边界与已知限制

- macOS 实机、Linux 客户端原生只读、真实多用户 ACL、多人听感未执行；本轮也未重跑真实音频设备相关的人工听感。
- 未在隔离发行副本中交互启动 GUI 主程序；发行边界通过目录／ZIP 逐文件校验，真实界面由 `gui_regressions`／`voice_plugin_panel`／`plugin_services` 覆盖。
- 归属记录损坏时没有 GUI 修复入口：自动迁移会一直保留旧目录并提示，需要人工删除或修正该记录后才会继续。这是有意的保守行为，记录在此以免误认为自动恢复。
- 无法认证的包（损坏、签名不符）会让该安装目录内的显示名自动迁移暂停，直到移除或替换该包；旧目录与新数据都保留、不会丢失，但在该包处理前看不到旧显示名目录里的配置。
- 设置迁移未结算期间，本次运行的宿主设置（主题、窗口尺寸、列宽、插件启用状态）不会持久化，只显示提示；下次启动重试成功后恢复写入。
