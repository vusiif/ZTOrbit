# 客户端存储与语音阶段复核（2026-10-21）

结论：**尚未达到阶段完成条件，暂不应清理根目录的 GOAL.md、TODO.md。** 两份任务文件中的完成标记过于乐观。本次发现 6 项 P1 收口问题和 1 项 P2 需求偏差，其中 6 项源码问题已用隔离探针复现，另 1 项发行物问题已用 SDK 内容指纹和实际文件核实。

核对依据是本阶段的存储、插件清理及语音设计要求（R01–R10、P1–P6）和 T01–T18 修复清单；本文保留这些编号及发现，不依赖根任务文件一直存在。未修复业务代码、未修改版本、未重新发布 SDK 或插件包，也未执行文件清理。此前的未提交修改全部保留。

环境：Windows 11 x64、MSVC 2022、Qt 6.8.3、Release；客户端 0.5.1-alpha／API 0.5-alpha，服务端 0.2.1-beta／API 0.2-beta。

## 需要修复的发现

### A01 — P1：部分迁移失败后，仍允许新配置占据待恢复目标

对应要求：个人根回退迁移按项重试，未恢复项禁止以默认值覆盖（设计 §3.2，T01）。

位置：`src/client/main/storage/StorageContext.cpp` 的 `prepare()`、`migrateSharedData()`、`migrateItem()`；`src/client/main/plugins/HostServicesImpl.cpp` 的构造与 `updateFile()`。

`prepare()` 忽略 `migrateSharedData()` 的失败，仍返回成功；`m_migrationComplete` 只被赋值，未用于限制相应设置／插件文件的写入。插件可对尚未复制的 `config.json` 正常执行初始化。重试时，`migrateItem()` 见到新目标已经存在就保留目标，并把该项视为完成，原配置不会恢复。

复现：在共享根准备 `id-field-probe/config.json={"nickname":"original","volume":123}`，用 Windows 独占文件句柄暂时阻止读取，并通过根 `cache` 文件占位选择个人根。第一次 `prepare()` 返回成功而迁移 `completed=false`。运行真实的 `storage_update_probe --json-field <个人 plugins-data> config.json nickname default 1`，写入成功。释放来源句柄并再次 `prepare()`，迁移变成 `completed=true`，最终只剩 `{"nickname":"default"}`，原 `volume` 丢失于活动配置。

证据：[storage-probe.json](storage-probe.json) 的 `partialMigrationDefaultWrite`。

修复应按迁移项保护写入；插件对应项未恢复时应明确阻止持久写入或初始化。增加“复制失败 → 正常消费者初始化 → 重启重试”的完整回归，不能仅调用两次 `prepare()`。

### A02 — P1：显示名可使旧目录迁移越过 plugins-data 边界

对应要求：显示名只参与受控历史迁移，路径不能越界（设计 §3.1、§4.2，T15）。

位置：`src/client/main/storage/PluginDataStore.cpp:124`、`:207`、`:224`。

`legacyCandidates()` 接受未校验的显示名，`prepare()` 直接将其交给 `QDir(m_pluginsDataRoot).filePath(name)` 并 `rename()`。包和注册表只要求显示名非空，不能阻止带 `../` 或绝对路径的名称到达此处。文件 API 的相对路径校验不覆盖迁移路径。

复现：在 `plugins-data` 的同级创建 `outside/keep.txt`，调用 `prepare("audit", "../outside", error)`。结果没有错误，原 `outside` 消失，其文件被搬进 `plugins-data/id-audit/`。以后显式删除该插件就可能删除这份原本不属于插件的数据。

证据：[storage-probe.json](storage-probe.json) 的 `displayNameTraversal`。

修复应只允许历史显示名单一目录名，并重新验证来源的规范化父目录及链接边界；不能重新限制正常显示名本身。

### A03 — P1：实际生成的待删记录无法被自身恢复入口接受

对应要求：已确认删除的部分失败可在重启后继续（设计 §5.3，T03）。

位置：`src/client/main/plugins/PluginLoader.cpp:382`、`:416`、`:551`。

删除记录的 `dataRoot` 写成 `root.absolutePath()`。这里 `root` 已经是 `plugins-data` 的父目录，因此实际写的是活动根的父目录。恢复入口却要求该字段等于 `pluginDataRoot`，所以正常删除留下的记录会被判为“记录属于另一个插件数据根”。

复现：扫描一个禁用但认证成功的真实测试包，在移入待删区后用 Windows 文件句柄暂时阻止删包。正常 `removeFile()` 删除数据并留下 `dataDeleted` 记录；释放占用后调用 `recoverPendingDeletions()`，返回 0，记录仍在，错误正是数据根不匹配。

证据：[deletion-probe.json](deletion-probe.json) 的 `generatedDeletionRecord`。

既有 `pendingDeletionsRecoverEveryStage` 测试手工构造了正确的 `dataRoot`，所以不会发现生产写入端的错误。应补充“正常删除生成记录 → 恢复”的往返测试。

### A04 — P1：启动恢复绕过包变更锁，也未复查新的安装引用

对应要求：恢复已确认操作仍须遵守包锁 → 数据锁顺序，并保留其他同 ID 安装引用的数据（设计 §5.2、§5.3，T02、T03）。

位置：`src/client/main/plugins/PluginLoader.cpp:514` 至 `:625`，尤其 `:575`、`:600`。

`recoverPendingDeletions()` 不获取 `PackageChangeLock`，读记录、移包和删包也不处于同一数据协调事务中。它直接信任旧记录的 `referenced=false`，没有重新扫描已安装包；恢复发生在应用首次插件扫描之前。

复现：创建一个正确的 `moved` 记录和待删包，同时在安装目录放入重新安装的同 ID 真实包，持有安装目录 `.import.lock` 后调用恢复。恢复仍返回 1，重新安装的包还在，配置数据却已经被删除。

证据：[deletion-probe.json](deletion-probe.json) 的 `recoveryWithReinstalledPackageAndHeldLock`。

恢复应采用与正常删除相同的协调协议，重新认证当前安装引用及所有权，并在锁竞争或归属不确定时保留。测试应分别覆盖包锁占用、重新安装、另一 profile 并发导入和活租约下原包不能先被移走。

### A05 — P1：同名不同 ID 的首次扫描仍可能错误认领旧数据

对应要求：同名对应多个 ID 时保留旧目录，不自动迁移（设计 §4.2）。

位置：`src/client/main/storage/PluginDataStore.cpp:136`、`:169`、`:220`；`PluginLoader` 创建文件服务的调用处。

`nameClaimedByOther()` 只检查已经写入的 `.ztgui/plugins/*.json`，并未使用完整安装清单中的显示名／ID 归属。若两个同名包都尚未登记，先初始化的插件会立即搬走同名旧目录，第二个插件到来时已无法发现原始冲突。

复现：准备 `plugins-data/Shared Name/keep.json`，依次对 `first-id` 和 `second-id` 调用 `prepare()`，显示名都为 `Shared Name`。两个调用均成功，第一个 ID 拿走旧数据，原目录消失，第二个 ID 没有这些数据。真实加载器也没有在此前向存储层登记全部扫描到的名称归属。

证据：[storage-probe.json](storage-probe.json) 的 `sameNameLegacyClaim`。

应在任何自动旧目录迁移之前，用完整认证安装清单建立候选归属；含禁用、不兼容等已安装包。测试应以两个真实同名包与一份旧目录验证，不能只验证两个空 ID 目录可以共存。

### A06 — P1：发行包与 SDK 尚未包含跟进修复

对应要求：最终源码、SDK、媒体／语音包与隔离发行验证一致（设计 P6、§10.2，T12、T18）。

当前发行目录仍是 2026-10-18 20:00 左右的产物；关键宿主存储、包缓存、语音配置和媒体源码随后才被修改。此前跟进验证已明确记录“未重新打包”，10 月 19 日也没有更新发行包；根任务状态却声明产物均已更新。

实际核对，原始指纹与哈希见 [artifact-audit.json](artifact-audit.json)：

- `build/dist/ZTGUI-api-v0.5-alpha/sdk-tools.json` 中的 `src/shared/package/PluginPackage.cpp` 和 `.h` 来源 SHA-256 与当前源码不一致。构建技能把共享包实现纳入 SDK 输入；独立插件构建脚本会检查并拒绝这样的过期 SDK，不能以“公开头未变”判定 SDK 可复用。
- 本地媒体 worker 包 SHA-256 为 `6D226B8AE73FC10A2F8BB2B9083C081EC3ED752EDA7330E09EDFBE65888979BD`，发行媒体包为 `58B22D8ED7BC4616D999FAE4A7EABF63C9628C12C269025E7FDD6F2D30259FBB`；T09 的新终态与尾音专项曾使用本地包，不能套用到发行包。
- 发行语音包和其中间构建包均早于当前 `VoiceConfig.cpp` 的旧源键清除修复；两包哈希相同只表明它们相互一致，不表明当前源码已经交付。
- 本次增量构建更新的是测试构建目录里的程序，不会更新 `build/dist/windows-x64`、其 ZIP 或 SDK。

修复完成后，应按“宿主 SDK → 媒体 SDK／包 → 语音消费者／包”的顺序重新交付，记录输入指纹及产物哈希，再对仅有发行目录的隔离副本验收。不需要改变版本号或无关服务端业务。

### A07 — P2：个人根迁移复制了插件日志和可丢弃缓存

对应要求：个人根只迁移持久设置／业务数据／归属，日志留在原处，不复制可丢弃缓存（设计 §3.2，T01）。

位置：`src/client/main/storage/StorageContext.cpp:480`、`:587`。

插件目录使用没有排除规则的递归复制，因而 `id-*/cache/` 与 `id-*/logs/` 一并复制。根级 `logs/`、`cache/` 不复制的既有测试不能代表插件子目录也正确排除。

复现：在共享插件目录同时准备配置、`cache/throwaway.bin` 和 `logs/old.log`，选择个人根后，后二者均在个人根出现。

证据：[storage-probe.json](storage-probe.json) 的 `fallbackCopyScope`。

应按迁移类别过滤目录树，并增加插件子目录级的排除测试。

## 本轮验证

先增量重建当前源码对应目标，再运行回归。构建与探针均位于 `build/`；探针的文件树使用 `QTemporaryDir`，结束后自动清除。旧设置来源使用临时测试路径，未迁移或清理用户真实设置。探针是本次复核用途，尚未作为正式回归测试纳入 `src/tests/`。

已通过：

| 验证 | 结果 |
| --- | --- |
| `plugin_package_container`、`storage_core`、`member_icmp_ping` | CTest 3/3 通过 |
| `core_regressions` | 通过 |
| `dynamic_plugins` | 通过 |
| `gui_regressions` | 通过；32 passed，0 failed，3 skipped；[输出](gui-output.txt) |
| `gpu_acceleration_override` | 通过；3 passed，0 failed |
| `windows_native_readonly` | 通过；`elevated=False`；[输出](readonly-output.txt) |
| `voice_plugin_panel` | CTest 通过，198.48 秒；使用既有发行包，不能据此关闭 A06 |
| `plugin_services` | CTest 通过，49.93 秒 |
| `voice_ui_tests` | 21 passed，0 failed，0 skipped；[输出](voice-ui-output.txt) |
| `media_engine_tests` | 19 passed，0 failed，0 skipped；[输出](media-engine-output.txt) |
| 隔离存储探针 | 退出码 0；复现 A01、A02、A05、A07 |
| 隔离删除探针 | 退出码 0；复现 A03、A04 |

探针退出码 0 表示完成了复现，不代表目标行为正确。原有回归通过与新缺口并不矛盾：已有测试缺少迁移失败后消费者写入、生产记录往返、恢复期锁／重装引用，以及首次同名归属等场景。

原生只读测试首次在启动脚本阶段因 `Microsoft.PowerShell.Security` 模块自动加载失败而未执行 ACL 验证；只对该次复测的子进程将 `PSModulePath` 限定为 Windows PowerShell 系统模块目录后通过。未修改系统环境或权限策略；首次启动遗留的两个空临时目录已清除。

最后一组 CTest 的[原始输出](plugin-suite-output.txt)保留了该次只读脚本启动失败，以及随后通过的真实插件面板／服务测试；因此该组首次汇总为 2/3。独立只读复测通过的输出另行保留，不把首次汇总改写成全绿。累计 10 个不同 CTest 场景最终均通过，另有语音 UI／媒体引擎专项通过。

主要执行入口（均先以 `cmake --build ... --config Release --target ...` 更新相应目标）：

```powershell
ctest --test-dir build/cmake-build-plugin-tests -C Release -R '^(storage_core|plugin_package_container|member_icmp_ping)$' --output-on-failure
ctest --test-dir build/cmake-build-plugin-tests -C Release -R '^core_regressions$' --output-on-failure
ctest --test-dir build/cmake-build-plugin-tests -C Release -R '^dynamic_plugins$' --output-on-failure
ctest --test-dir build/cmake-build-plugin-tests -C Release -R '^(gui_regressions|gpu_acceleration_override)$' --output-on-failure
# 实际插件面板运行时设置 ZTGUI_PREVIEW_PLUGIN 指向已独立构建的预览消费者。
ctest --test-dir build/cmake-build-plugin-tests -C Release -R '^(windows_native_readonly|plugin_services|voice_plugin_panel)$' --output-on-failure
# 只读复测：只限定当前启动子进程的模块路径。
$env:PSModulePath = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/Modules'
ctest --test-dir build/cmake-build-plugin-tests -C Release -R '^windows_native_readonly$' -V
```

Qt 的 PATH、插件及 QML 导入目录指向 `build/cmake-build-tools/Qt/6.8.3/msvc2022_64`；媒体引擎测试另将 `build/cmake-build-media-dependencies/installed/bin` 放在 PATH 前。语音 UI 和媒体引擎通过构建技能的 `run-media-tests.ps1` 包装执行，读取真实退出码并保存标准输出。

本次未执行 macOS 实机、Linux 客户端原生只读、真实多用户 ACL／多人听感、物理设备拔插、真实进程崩溃注入。它们仍为验证边界；本次否定收口的直接依据是上述已复现问题和发行物差异。

## 阶段与清理建议

- R01 成员显示、R02／R03 提示音主体、R10 插件自管配置已有实现和相关回归证据；无需重写。
- R04–R09 的正常路径已有实现，但迁移保护、归属及删除恢复尚有上述缺口；P6 交付一致性尚未完成。
- 先修复 A01–A06 并补对应回归，处理 A07；最后更新产物和独立 SDK／隔离发行证据，再重新判断收口。
- 暂时保留根 GOAL.md、TODO.md。可以在后续清理时移除一次性任务文件，但先将仍待办事项保留在明确入口；历史验证记录、当前 API／维护／插件文档应保留。
- 现有两份跟进验证记录含指向根 TODO.md 的 Markdown 链接；最终删除任务文件时需同时调整链接，避免留下断链。

本记录是对旧“已完成”结论的追加复核，不改写旧验证证据。
