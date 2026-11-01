# QML 前端维护约定

应用窗口、页面、菜单、弹窗、表格及指南均使用 Qt Quick/QML，入口在 `src/client/main/qml/Main.qml`。`AppController` 接受界面操作，`SystemOperations` 保留异步平台授权与脚本流程，`RecordModel` 按稳定ID增量更新。网络／成员数据不能使用每秒重置整个模型的实现；成员使用独立模型，避免刷新销毁备注编辑器。轮询活跃状态由窗口可见性、导航页面与Moon弹窗显式控制。

深浅主题沿用原有配色，Qt Quick Controls 使用Basic样式与应用palette。语音QML页面继承窗口palette，不导入宿主内部组件。现有 `names/`、`voice-chat/` 和主题设置保留；QML表格列宽使用 `tables/qml/`，窗口尺寸使用 `window/qml/`，不覆盖旧Widgets序列化布局。这些键由 `backend.hostSetting()`／`backend.setHostSetting()` 通过宿主 `SettingsStore` 读写同一 `settings.ini`；QML 不再使用 `Settings` 元素，因为它在加载时就会把默认属性写入文件，绕过宿主对未恢复设置文件的写入保护。

## 插件与引擎寿命

公开页面工厂接收借用的 `QQmlEngine*` 和 `QQuickItem* parent`，返回QObject父对象与可视父项均为parent的新页面。SDK的 `createQmlPage()` 在组件完成之前设置父项和初始属性，插件自己的QML存入DLL资源。控制器通过required属性注入，并由页面持有；传入QVariant时转换为 `QObject*`，避免注册DLL所有的控制器指针元类型。不得注册DLL所有的全局QML类型、单例或宿主内部类型。

退出先取消并等待初始化和系统操作，保持GUI事件循环运行；然后销毁页面／控制器、清理引擎JS对象与组件缓存，释放工厂、查询和插件库。直接析构仍有阻塞等待兜底。窗口标题栏关闭在Windows收起到托盘，任务栏关闭和明确退出等待清理完成；没有托盘时最小化到任务栏。

客户端入口和生命周期IID使用 `/3.0` 区分页面ABI，旧Widgets `/1.0` 与旧QML `/2.0` DLL在实例化前拒绝。客户端 `VERSION` 及派生的 API 版本由开发者管理；服务端版本由客户端侧 `SERVER_VERSION` 指定并独立演进。服务端已迁移为 Java 25 + Netty 的独立宿主，不再共享客户端 C++ 插件 ABI，接口与部署见[服务端 API](server-api.md)。

## 构建与部署

主程序使用 `qt_add_qml_module` 编译并嵌入QML；Qt 6.8/MSBuild的测试目标引用外部源目录会生成含 `_..` 的缓存路径，因此测试资源使用 `NO_CACHEGEN`，生产目标仍生成QML缓存。Widgets仅作为Qt.labs.platform原生托盘／菜单后备库，公开SDK和语音页面不依赖它。

宿主显式导入 `QtQuick.Controls.Basic`，并继续为通用Controls插件设置Basic。弹窗、行菜单和编辑器按需构造；已打开的弹窗保持缓存，以保留再次打开时的状态。成员子模型在首次拿到成员时创建。隐藏窗口允许释放场景图／图形设备资源，恢复后重建；业务模型、备注、导航和语音会话保留。设置中的“禁止GPU加速”及 `--disable-gpu-acceleration` 在创建QML引擎前选择Qt的software场景图；设置默认开启，首次切换时提示完全退出并重启后生效，启动参数仅覆盖本次启动。

Windows的 `windeployqt --qmldir src/client/main/qml` 和macOS的 `macdeployqt -qmldir=...` 必须部署QML模块及其插件；仅扫描EXE的DLL导入表不足以部署QML。插件的额外导入模块也需要打包；当前语音页使用宿主已有的 Qt Quick Controls，音频通过媒体 API 访问 `media.session`，实现位于媒体工作进程，语音插件不导入 Multimedia 或 FFmpeg。

## 验证范围

回归分为QML窗口交互、核心业务、动态插件、网络协议、Windows端口、语音页面及音频。QML测试使用真实Quick窗口／鼠标／键盘事件，并保存主题、尺寸和指南截图。Windows托盘与原生焦点另用windows平台验证。测试使用模拟CLI与临时配置目录，不执行真实安装、卸载或修改ZeroTier配置。

macOS代码与部署参数更新不能替代实机验证；真实多人麦克风／耳机听感同样需要人工验收。交付时列明实际验证平台、范围和未验证项。
