# A01–A07 修复与发行物重交付（2026-10-21）

范围：修复 [goal-audit-2026-10-21](../audit-2026-10-21/README.md) 的 A01–A07，并补充对应回归；随后按“宿主 SDK → 媒体 SDK／包 → 语音消费者／包 → 主程序”的顺序重新交付。旧审计记录和更早的验证原文保留，本记录作为更正和补充，不改写旧证据。

环境：Windows 11 x64，MSVC 2022，Qt 6.8.3（`build/cmake-build-tools/Qt/6.8.3/msvc2022_64`），Release；客户端 `0.5.1-alpha`／API `0.5-alpha`，服务端 `0.2.1-beta`／API `0.2-beta`。未修改 `VERSION`、`SERVER_VERSION` 或插件版本。

## 修复与回归

| 发现 | 修复 | 回归 |
| --- | --- | --- |
| A01 迁移失败后默认值占据目标 | `StorageContext::migrationComplete()`／`migrationCorrupt()` 暴露真实状态；`PluginDataStore::pendingMigration()` 按 `migration.json` 的未结算 `plugin-data` 项匹配稳定 ID 目录和旧显示名；`HostServicesImpl::writeFile()`／`updateFile()` 拒绝对配置和业务文件的持久写入（`cache/`、`logs/` 除外），错误说明“共享数据尚未恢复”；记录损坏时同样暂停写入。 | `StorageTests::pendingMigrationProtectsPluginWritesUntilItSettles`；`CoreTests::pendingMigrationBlocksConsumerInitializationUntilRetry` 用 Windows 独占句柄阻止来源读取，第一次 `prepare()` 为 partial，真实 `HostServicesImpl` 初始化后写入被拒绝，释放后重启复制成功并读到原 `nickname`／`volume`。 |
| A02 显示名越界迁移 | `PluginDataStore::legacyCandidates()` 只接受单一目录名（拒绝 `/`、`\`、`:`、绝对路径、`.`／`..`、非法文件名）；`prepare()` 重命名前复核来源规范化父目录必须是 `plugins-data` 且文件名与候选一致；显示名本身不限制字符集，仅用于展示和归属记录。 | `StorageTests::legacyDisplayNameNeverEscapesPluginsData`（`../outside`、绝对路径、反斜杠名称均不移动外部数据，并给出提示）。 |
| A03 待删记录无法被自身恢复 | `removeFile()` 的 `dataRoot` 改写为真实插件数据根；记录新增授权时包文件 SHA-256。 | `LoaderTests::generatedDeletionRecordCompletesOnRestart`：正常删除路径在包只读时留下真实记录，断言 `dataRoot` 等于插件数据根，重启恢复返回 1 并清理待删包与记录。 |
| A04 恢复绕过包锁、未复查安装引用 | `recoverPendingDeletions()` 全程持有安装目录 `PackageChangeLock`；重新认证当前 `.ztplugin` 清单判断同 ID 引用，重装包存在时保留数据；复核所有权记录、包哈希和租约；活租约下在 `recorded` 与 `moved` 两阶段都不先移走或删除原包；锁竞争时保留全部记录并报告。 | `LoaderTests::recoveryHonorsPackageLockAndReinstalledReferences`（子进程持锁、重装同 ID 包、同路径换包哈希不一致）；`LoaderTests::pendingDeletionsRecoverEveryStage` 新增活租约下 `recorded` 阶段不移包、释放后完成的用例，原有六阶段用例保持通过。 |
| A05 首次同名不同 ID 认领旧数据 | 插件扫描在后台发现任务中、任何实例启动前，用全部已认证安装包（含禁用、不兼容）调用 `PluginDataStore::registerInstalledNames()` 写入 `.ztgui/plugins/<id>.json` 的 `names`，不阻塞 GUI 线程；`nameClaimedByOther()` 因此能看到尚未启动的第二个 ID。 | `LoaderTests::sameNameInstallsDoNotClaimLegacyData`：两个真实同显示名（`Shared Name`）、不同 ID 的测试包 + 一份旧目录；扫描后两个归属记录都含该名称，依次初始化两个消费者均不移动旧目录。新增 CMake 夹具 `plugin_fixture_twin_a`／`twin_b`。 |
| A06 发行物过期 | 重新生成客户端 SDK、媒体 API／Engine SDK、媒体包、语音包和主程序；`sdk-tools.json` 已记录当前共享包源码哈希；媒体包与语音包的发行副本与各自中间构建逐字节一致。 | 见下方“产物与输入指纹”和 CTest 结果。 |
| A07 个人根复制插件日志／缓存 | `StorageContext::migrateItem()` 在 `plugin-data` 类别中按顶层 `cache/`、`logs/` 排除，并在迁移项记录 `excluded` 数量；根级日志／缓存原本就不复制。 | `StorageTests::personalMigrationSkipsPluginLogsAndDisposableCaches`（配置复制、`excluded=2`、来源保留）。 |

## 构建命令

```powershell
$QtRoot = (Resolve-Path "build/cmake-build-tools/Qt/6.8.3/msvc2022_64").Path
$ff = (Resolve-Path "build/cmake-build-media-dependencies/installed/bin").Path
$env:PATH = "$ff;$QtRoot/bin;$env:PATH"

cmake -S . -B build/cmake-build-plugin-tests -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_PREFIX_PATH=$QtRoot"
cmake --build build/cmake-build-plugin-tests --config Release --target `
    storage_tests core_tests plugin_loader_tests

# 发行物顺序：宿主 SDK/主程序 → 媒体 SDK/包 → 语音包。
./.agents/skills/ztgui-build/scripts/build-windows.ps1 -QtRoot $QtRoot -RebuildApi
./.agents/skills/ztgui-build/scripts/build-media-windows.ps1 -QtRoot $QtRoot `
    -ApiSdk build/dist/ZTGUI-api-v0.5-alpha `
    -FfmpegRoot build/cmake-build-media-dependencies/installed
./.agents/skills/ztgui-build/scripts/build-plugin-windows.ps1 `
    -PluginDirectory src/plugins/voice-chat/client -Target ztgui_voice_chat -QtRoot $QtRoot `
    -ApiSdk build/dist/ZTGUI-api-v0.5-alpha -AdditionalSdk build/dist/ZTGUI-media-api-v2
```

## 自动化结果

CTest（`build/cmake-build-plugin-tests`，Release）：

```text
plugin_package_container    Passed    1.95 sec
storage_core                Passed    3.52 sec
member_icmp_ping            Passed    4.85 sec
windows_native_readonly     Passed    0.89 sec
gui_regressions             Passed   84.13 sec
gpu_acceleration_override   Passed    0.95 sec
core_regressions            Passed   20.26 sec
dynamic_plugins             Passed   26.56 sec
voice_plugin_panel          Passed  209.20 sec
plugin_services             Passed   50.09 sec
100% tests passed, 0 tests failed out of 10
```

原始输出见 [ctest-output.txt](ctest-output.txt)。

直接运行计数：

| 测试 | 结果 |
| --- | --- |
| `storage_tests` | 24 passed、0 failed（原 21，新增 3）；[输出](storage-output.txt) |
| `core_tests` | 27 passed、0 failed（原 26，新增迁移消费者用例 1）；[输出](core-output.txt) |
| `plugin_loader_tests` | 28 passed、0 failed（原 25，新增 3）；[输出](loader-output.txt) |
| `media_engine_tests` | 19 passed、0 failed，退出码 0；[输出](media-engine-output.txt) |
| `media_worker_integration` | Passed，96.73 秒；[输出](media-integration-output.txt) |
| `voice_plugin_tests`／`voice_ui_tests` | 重编了 `voice_plugin_tests` 但本轮未单独运行；`voice_ui_tests` 构建目录未重编；`voice_plugin_panel` CTest 使用新发行语音包通过 |

媒体引擎测试由本技能 `run-media-tests.ps1` 包装执行时，包装器在本机 PowerShell 5.1 上把已正常退出的进程报告为空 `ExitCode`；随后用同一环境直接运行同一可执行文件，输出为 `19 passed, 0 failed` 且 `$LASTEXITCODE=0`。记录该包装器差异，不把它计为代码失败。

## 产物与输入指纹

宿主 SDK 的 `build/dist/ZTGUI-api-v0.5-alpha/sdk-tools.json` 已与当前源码一致：

| 来源 | 记录／实际 SHA-256 |
| --- | --- |
| `src/shared/package/PluginPackage.cpp` | `4509c64275e3d83682db3d9434192f54652c8541772946ffabbfc234fa832516` |
| `src/shared/package/PluginPackage.h` | `ba5216e52e2f5f1d61d41d8f952e15b5d0aaed001d954afc95ec330248e888fe` |

产物（SHA-256）：

| 产物 | SHA-256 |
| --- | --- |
| `build/dist/windows-x64/ZTGUI.exe` | `ACA6289804CAF4289C3FA0640781CD9877F4469D6556787FE4F16A2593ED2D20` |
| `build/dist/windows-x64.zip` | `AFD50A4C16682997A3F13888A92C54544B92CC8DE1131A46846F12255F84B480` |
| `build/dist/ZTGUI-api-v0.5-alpha.zip` | `C5515C0A3A843D5AEA4823EF3BF793A1BCF3AE2A872667B85587979E2B47BC71` |
| `build/dist/ZTGUI-media-api-v2.zip` | `D84D15986CFECB1D075A686D95AE0ED6E9320D58DE8D0EEF0D4EA6F1BF0C0408` |
| `build/dist/ZTGUI-media-engine-sdk-v1.zip` | `4CCEE29D9029EB3790F334B54EA7A59826F316422B535DD97FCDD3747431DA1A` |
| `build/dist/plugins/ztgui_media_worker-v2.0.0.ztplugin` | `A67EAAB57EC3769EE21A7584AFB7209B5B814CAAAEED5DB84A4AC21811B8B0AC` |
| `build/cmake-build-media-v2-worker/Release/ztgui_media_worker.ztplugin` | 同上（发行副本与中间构建逐字节一致） |
| `build/dist/plugins/ztgui_voice_chat-v0.3.0.ztplugin` | `9D734EBD25A113995F7E1A37D4573CCD3D705CB4F365DEF2A3BB40EA11BC1057` |
| `build/cmake-build-plugin-ztgui_voice_chat-windows-x64/Release/ztgui_voice_chat.ztplugin` | 同上（发行副本与中间构建逐字节一致） |

原审计中的旧值不再是当前产物：媒体包原 `58B22D8E…`、语音包原 `763CFA1C…`、主程序原 `04DDE403…` 均已被本轮重建覆盖。

发行目录校验：

- `build/dist/windows-x64/` 与 `windows-x64.zip` 解压副本逐文件比对：191/191 个文件、路径与内容一致。
- 主程序目录和 ZIP 不含 `plugins/` 目录、FFmpeg／avcodec／Qt Multimedia 运行库或业务包，符合主程序与插件分离边界。
- `sdk-tools.json` 的 44 个来源记录全部与当前源码一致（0 个不一致）；SDK `include/` 与 `src/client/api/include/` 逐文件哈希一致（0 个不一致），独立插件构建不会再因过期 SDK 被拒绝。
- 最新源码修改时间为 `2026-10-21T15:58:05`（`PluginLoader.cpp`，包含把名称登记移入后台发现任务的调整）；SDK 记录 `15:45:25`、媒体包 `15:47:01`、语音包 `15:47:41`、主程序 `16:06:46`、主程序 ZIP `16:06:58`。名称登记调整只改宿主私有实现，SDK 指纹未变并复用；媒体／语音包不包含宿主加载器，因此未重编；主程序已重新构建，所有发行物均晚于其包含的源码。

## 媒体与语音

- 媒体引擎：`19 passed、0 failed、0 skipped`（直接执行退出码 0），见 [media-engine-output.txt](media-engine-output.txt)。
- 媒体 worker 集成：CTest `media_worker_integration` 通过（96.73 秒），见 [media-integration-output.txt](media-integration-output.txt)。
- 语音面板：`voice_plugin_panel` CTest 使用本轮新语音包与新媒体包通过（209.20 秒）；真实服务器多人听感、物理设备拔插仍未由本记录验证。

## 未验证边界

- macOS 实机、Linux 客户端原生只读、真实多用户 ACL 与多人听感未执行。
- 未注入真实进程崩溃；待删恢复通过记录阶段、只读包和锁竞争构造。
- 未在隔离副本中交互启动 GUI 主程序（仅校验 ZIP 与目录逐文件一致及启动依赖边界）；主程序实际加载新发行语音／媒体包由 CTest `voice_plugin_panel`／`plugin_services` 覆盖。
- `run-media-tests.ps1` 在本机 PowerShell 5.1 的退出码为空属包装器行为，已用直接运行复核，未修改构建脚本。

## 后续

[再次复核](recheck.md) 指出的 A01、A04、A05 三个剩余 P1 缺口已在[同日第二轮修复](remaining-fixes.md)中关闭（宿主 INI 写入保护、恢复协调锁区间、归属内存清单），主程序已重新构建；本记录的产物哈希与文件数不再是当前值，旧证据保留。
