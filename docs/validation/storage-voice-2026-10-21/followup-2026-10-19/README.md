# 存储、语音与验收收口验证（2026-10-19）

范围：完成 [阶段归档](../README.md) 的 **T15–T18**，即文件服务与显示名解耦（T15）、提示音事件测试补齐（T16）、验收矩阵缺口与原生只读检查（T17），以及文档／验证记录／`GOAL.md` 状态收口（T18）。T01–T14 的原始证据见 [2026-10-18 跟进记录](../followup-2026-10-18/README.md)，本记录不重写旧证据。

环境：Windows 11 x64，MSVC 2022，Qt 6.8.3（`build/cmake-build-tools/Qt/6.8.3/msvc2022_64`），Release；客户端 `0.5.1-alpha`／API `0.5-alpha`，服务端 `0.2.1-beta`／API `0.2-beta`。未修改 `VERSION`、`SERVER_VERSION` 或插件版本。原生只读检查以普通用户运行（脚本输出 `elevated=False`）。

## 构建

```powershell
$QtRoot = (Resolve-Path "build/cmake-build-tools/Qt/6.8.3/msvc2022_64").Path
$ff = (Resolve-Path "build/cmake-build-media-dependencies/installed/bin").Path
$env:PATH = "$ff;$QtRoot/bin;$env:PATH"

cmake -S . -B build/cmake-build-plugin-tests -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_PREFIX_PATH=$QtRoot"
cmake --build build/cmake-build-plugin-tests --config Release --target `
    storage_tests core_tests plugin_loader_tests zerotier_gui_tests `
    plugin_services_tests voice_plugin_tests readonly_probe

cmake --build build/cmake-build-plugin-ztgui_voice_chat-windows-x64 `
    --config Release --target voice_ui_tests
```

全部目标构建成功。`readonly_probe` 与 `storage_update_probe` 随宿主测试目标生成；`storage_update_probe` 本轮新增 `--json-field`、`--remark` 两个跨进程探针模式。

## 自动化结果

```powershell
$env:QT_PLUGIN_PATH = "$QtRoot/plugins"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$QtRoot/plugins/platforms"
$env:QML_IMPORT_PATH = "$QtRoot/qml"
$env:QML2_IMPORT_PATH = "$QtRoot/qml"
$env:QT_QUICK_BACKEND = "software"
ctest --test-dir build/cmake-build-plugin-tests -C Release -R `
    "plugin_package_container|network_transport|storage_core|member_icmp_ping|windows_native_readonly|gui_regressions|core_regressions|dynamic_plugins|plugin_services" `
    --output-on-failure
```

结果：`100% tests passed, 0 tests failed out of 9`。直接运行计数：

| 测试 | 结果 |
| --- | --- |
| `storage_tests` | 21 passed、0 failed（原 20，新增不可读租约 1） |
| `core_tests` | 26 passed、0 failed（原 25，新增跨进程字段／备注 1；文件服务用例扩展） |
| `plugin_loader_tests` | 25 passed、0 failed（原 23，新增禁用／不兼容／加载失败保护与删除失败阶段 2） |
| `zerotier_gui_tests -platform offscreen` | 32 passed、0 failed、3 skipped（沿用既有平台跳过） |
| `plugin_package_tests` | 11 passed、0 failed |
| `plugin_services_tests -platform windows` | 25 passed、0 failed |
| `windows_native_readonly` | 通过；`elevated=False` |
| `voice_ui_tests -platform offscreen` | 21 passed、0 failed（原 19，新增 T16 两项） |
| `voice_plugin_tests -platform windows`（设置 `ZTGUI_PREVIEW_PLUGIN`） | 7 passed、0 failed、2 skipped（真实服务器／听感） |

### 稳定性说明

- 组合运行 `relay_end_to_end` 时出现一次 `primary.forwards-primaryBefore` 为 0 的失败；随后独立连续运行 3 次均为 `4 passed`。该测试与本次改动无关，按既有网络时序波动记录，未修改测试。
- 设置 `ZTGUI_PREVIEW_PLUGIN` 后首次完整运行 `voice_plugin_tests` 时 `panelOnlyConnectsOnClick` 因弹窗时序失败一次；单独运行 2 次及随后完整运行均通过（7 passed／2 skipped）。按既有 GUI 时序波动记录，未修改测试。
- 早先一次 `voice_plugin_tests` 手动运行遗漏 `QML_IMPORT_PATH`，导致 `QtQuick.Layouts` 缺失；补齐环境后全部通过，不把环境缺失计为代码失败。

## T15 文件服务与显示名解耦

实现：

- `HostServicesImpl` 的稳定 ID 成为唯一数据目录来源；构造时 ID 无效或缺失立即设置 `m_storageError`（“插件 ID 无效，无法建立数据目录”），不再回退到 `plugins-data/<显示名>`，也不会创建该目录。
- `filePath()` 不再检查显示名字符集；相对路径按分段校验，保留绝对路径、盘符／冒号、`.`／`..`、空段、末尾句点或空格、Windows 保留设备名（`CON.txt`、`NUL` 等）、符号链接／junction、非普通文件的防护。
- 加载器原有的 `storageReady()` 检查继续把该错误作为插件加载失败原因，插件不会被半初始化。
- 日志、缓存和普通业务文件都写入 `plugins-data/id-<小写 ID>/`，显示名含 `:`、`*`、末尾 `.` 等字符不影响读写。

测试：`CoreTests::pluginFilesUseDataRootAndRejectEscapes` 扩展为：

- 以显示名 `语音聊天`、稳定 ID `voice-chat` 读写成功，目录为 `id-voice-chat`，不存在 `plugins-data/语音聊天`。
- 以 `../escape`、`..`、`chat/other`、`chat:`、`CON.txt`、末尾点、末尾空格、空 ID、`builtin`、中文 ID 构造 `HostServicesImpl` 时 `storageReady()==false`、`dataDirectory()` 为空、读写失败，且不会创建显示名目录。
- 合法 ID `CON` 仍安全落到 `id-con`；相对路径中的 `CON.txt`、`example/NUL`、`example/file1.txt. ` 被拒绝。
- 继续覆盖绝对路径、`..`、链接根与链接目录（Unix 条件分支）。

未验证：macOS 上带特殊字符显示名的原生文件系统行为；本轮用普通 Windows 临时目录与直接构造的宿主服务验证，未再用一个真实 `.ztplugin` 的显示名走完整加载器路径。

## T16 提示音事件测试补齐

实现（测试侧）：在 `VoiceUiTests.cpp` 增加 `VoiceServerFixture`，真实监听本机 TCP、解析 `ZTS1` 帧并应答 `hello`、`join`／`create`；支持成功、`room_not_found` 失败和忽略（模拟挂起）三种房间响应，可复用同一 session ID。

新增用例：

- `selfJoinedCueRequiresSuccessfulJoin`：首次成功加入只触发一次 `selfJoined` 和一次加入音；首份成员快照中的远端成员不额外响；失败响应不响、不改变次数；请求挂起时断开连接按取消处理、不响；重连复用同一 session ID 后再次成功加入只响新的一次。
- `memberCuesFollowBaselineResetAndReconnect`：首份快照只建基线；重复快照、改名、静音、排序变化不响；同人数一进一出分别产生加入／离开事件；突发时加入播放、离开排队；`roomReset` 清空当前播放与待播队列，旧 clip 的 `finished` 不会重新启动播放；重连后首份快照只重建基线；停止后迟到的终态事件被忽略。

结果：`voice_ui_tests` 21 passed、0 failed。

未验证：真实服务器多人房间与听感；本用例只验证协议层事件与提示音调用，不验证音频设备实际发声。

## T17 验收矩阵缺口与原生只读检查

### 新增自动化证据

| 缺口 | 实现与测试 |
| --- | --- |
| 禁用／不兼容／加载失败包对无主判断的保护 | `LoaderTests::disabledAndIncompatiblePackagesProtectInstalledData`：分别用禁用、API 2.0 不兼容和 `onCreate` 失败的已安装包，断言 catalog 仍带认证后的稳定 ID、`hasUncertainPackages()==false`，`StorageMaintenance::orphans({id}, false)` 不把这些包的数据列为可删。 |
| 两进程分别改不同字段 | `StorageUpdateProbe --json-field` + `CoreTests::concurrentProcessesMergeDifferentFieldsAndRemarks`：两个真实进程各更新 `config.json` 的 `nickname`／`volume` 60 次，最终两个字段都保留且类型正确。 |
| 两进程分别改不同成员备注 | `StorageUpdateProbe --remark`（链接 `VoiceConfig.cpp` 的 `updateMemberRemark`）+ 同一测试：两个真实进程分别写入不同 ZeroTier ID 的备注，最终两条都保留。 |
| 删除标记失败 | `LoaderTests::deleteFailureStagesKeepConsistentState`：让 `.ztgui/pending-deletions` 成为普通文件，删除在记录阶段失败；断言包、数据未动、无记录。 |
| 移包失败 | 同一测试：让插件目录下 `.ztgui-pending-deletions` 成为普通文件；记录已写、移包失败；断言包与数据保留、记录被清理。 |
| 删数据失败 | 同一测试：把 `plugins-data/id-test-dynamic` 替换为 junction；删除因“不是普通目录”失败并把包移回，链接目标未被遍历或删除；移除 junction 后同一删除成功。 |
| 不可读／未知租约 | `StorageTests::unreadableLeaseBlocksMigrationAndDeletion`：损坏的租约 JSON 阻止旧显示名目录迁移和稳定 ID 数据删除；租约移除后可迁移、可删除。 |
| 日志 10 MiB 轮转 | 既有 `CoreTests::logRotationNotifiesCallback`（`app_log_probe --rotation` 实际写满 10 MiB，回调一次、两个文件）；本轮重新运行通过。 |
| 100 MiB 日志归属／256 MiB 缓存总量 | 既有 `StorageTests::retentionEnforcesByteLimits`（2×60 MiB 日志、2×130 MiB 缓存，删除最旧文件）；本轮重新运行通过。 |
| 伪造缓存索引／PID 复用 | 既有 `PluginPackageTests::cacheSessionOwnershipAndCrashRecovery`（未登记根、令牌不匹配、当前 PID + 伪造创建标识、活动 worker、结构越界）；`plugin_package_container` 本轮通过。 |
| 原生只读安装 | 新增 `readonly_probe.exe` + `src/tests/ReadOnlyProbe.ps1`，以受控 deny ACL（`CreateFiles,WriteData,AppendData`，容器与对象继承）拒绝共享目录写入，实际执行 `StorageContext::resolveForTesting()` 与 `prepare()`。CTest `windows_native_readonly` 输出：`elevated=False`、`mode=personal`、`prepared=true`、告警“程序目录不可写，已改用当前用户的个人存储目录”，且共享根没有生成 `settings.ini`。 |

### GOAL §10.1 验收矩阵映射

| 验收组 | 本轮证据 |
| --- | --- |
| 设置统一 | `core_regressions`、`gui_regressions`、`gpu_acceleration_override`（本轮通过） |
| 宿主迁移 | `storage_core`（含 T01 重试、损坏保留、目标优先） |
| 只读回退 | `storage_core` 故障注入 + `windows_native_readonly` 原生 deny ACL（非管理员） |
| 并发配置 | `core_regressions` 新增跨进程不同字段／不同备注；`storage_core` 协调锁用例 |
| ID 与旧目录 | `dynamic_plugins`、`storage_core`、`core_regressions` 的文件服务用例 |
| 语音配置 | `voice_ui_tests`（旧键清除、提交失败保留、设备规范化） |
| 删除协调 | `dynamic_plugins`（包锁、待删六阶段恢复、所有权、本次新增三阶段失败） |
| 无主清理 | `dynamic_plugins` 不确定包保护 + 禁用／不兼容／加载失败保护；`storage_core` 无主分类 |
| 缓存生命期 | `plugin_package_container`、`plugin_services`；真实崩溃注入仍未执行 |
| 清理安全 | `storage_core` 不可读租约；`plugin_package_container` 索引／PID／worker；原生只读 ACL |
| 日志与业务缓存 | `core_regressions` 轮转回调、`storage_core` 保留上限与自动调度 |
| 成员显示 | `member_icmp_ping`、`gui_regressions` |
| 提示音事件 | `voice_ui_tests` 的 T16 两项与既有队列／禁听用例 |
| 短音服务 | 本轮未重跑媒体源码测试；沿用 [2026-10-18 记录](../followup-2026-10-18/README.md) 的 `media_engine` 19 项与媒体集成 T09 证据 |
| 真实音频与卸载 | `plugin_services`、`voice_plugin_panel` 本轮重建后通过；真实听感、物理拔插未执行 |
| 包与 SDK | `plugin_package_container`；`voice_plugin_tests` 使用发布媒体 SDK 构建的预览消费者（`previewSdkConsumerRendersVideo` 通过）；宿主 SDK 消费者证据见 T12 旧记录，本轮未改公共头文件 |

### 原生只读检查命令

```powershell
ctest --test-dir build/cmake-build-plugin-tests -C Release -R windows_native_readonly -V
```

关键输出：

```text
elevated=False
{"activeRoot":"C:/Users/.../ztgui-readonly-.../user-data/installations/<installKey>/profile",
 "error":"","mode":"personal","prepared":true,"valid":true,
 "warnings":["程序目录不可写，已改用当前用户的个人存储目录。"]}
Windows native read-only fallback passed
```

POSIX 手动脚本 `src/tests/ReadOnlyProbe.sh` 已提供（普通用户 + `chmod 0555`，root 时跳过），本轮未在 Linux 执行；不作为已通过证据。

未验证：真实多用户 ACL、macOS 只读 `.app`、Linux 客户端原生权限；测试数据均使用临时根，未写入用户真实目录。

## 构建产物 SHA-256

```text
tests/Release/storage_tests.exe                  BA90A6437CD417B9230F47504A3E109C23845EE08C4961FAF5447BB4F7144875
Release/plugin_loader_tests.exe                  7E5537195741AFCC8EEE2BAA79B874B4E3E9747A6208C2526D810DFE401A4AC9
Release/core_tests.exe                           E244BF1263CFADA7E618E73B7D7BA4598327583985543C941FAB67A10CAD95EE
Release/zerotier_gui_tests.exe                   79FD7575A121405BF867B76DFD24420225707ED9E7754A5AA1428C642F6BA96B
Release/plugin_services_tests.exe                2545DCAB901D38CC8DA204FEB3E72BE854C4DA3B4FB194A04619F84E301C4BB0
Release/voice_plugin_tests.exe                   13D1550872203948CFAF0A1921C0623FF5E010461E49B4402FB3C5753632F08E
Release/voice_ui_tests.exe                       246090D5861491275A399CEF1B01F947B4E577E01AAB7C44637D054AE0844644
Release/readonly_probe.exe                       402526C87B7C902B4B7DAFBEB958D3C7A9D054C2F73DF4761F4701CB163FCDA7
Release/storage_update_probe.exe                 3BF7611B63C505CB87989EB32AC9BA67A9DB63B9DDC076D62EEC08A4F125E304
```

本轮未重编公共 SDK、媒体 API／媒体包或语音发行包；T15 只改宿主私有实现，T16 只改测试，T17 只增测试与探针。`build/dist/` 中的发行包与上一轮一致。

## 与旧验证记录的关系

- `docs/validation/storage-voice-2026-10-21/implementation-2026-10-18/README.md` 曾写 `plugin_loader_tests` 覆盖“租约阻止与待删包”，该表述早于实现；T02／T03 已补齐，当前为 25 项。旧记录原文保留，本记录作为更正。
- `docs/validation/storage-voice-2026-10-21/followup-2026-10-18/README.md` 末尾“T15–T18 未完成”已被本记录取代；T12 的 7 passed／2 skipped 在本轮重编后复现。
- `TODO.md` 的 T15–T18 已标记 `DONE`；`GOAL.md` 状态更新为完成，未执行边界以本记录为准。

## T18 文档收口

本轮同步更新：

- `docs/plugin-api.md`：显示名不参与路径校验、无效 ID 拒绝、相对路径防护。
- `docs/macos.md`：日志写入活动存储根，只读安装回退到个人 profile。
- `src/plugins/voice-chat/client/docs/voice-chat.md`：提示音事件测试覆盖。
- `docs/maintenance.md`、`docs/validation/README.md`：本记录索引与验证历史。
- `TODO.md`、`GOAL.md`、`CHANGELOG.md`：任务状态、完成状态与未发布变更。

仍未执行：macOS 实机、真实多用户并发、真实服务器多人听感、Linux 原生只读、媒体源码测试本轮未重跑。
