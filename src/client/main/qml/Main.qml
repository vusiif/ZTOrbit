import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import Qt.labs.platform as Platform

ApplicationWindow {
    id: window
    objectName: "mainWindow"
    required property var backend
    property string exportNetworkId: ""
    property string detailKind: ""
    property string detailId: ""
    property var aboutPlugin: ({})
    readonly property bool operationPending: backend.busy || backend.closing
    onOperationPendingChanged: {
        if (!notificationBanner) return
        if (operationPending) notificationBanner.present(backend.closing ? backend.shutdownMessage || "正在退出…" : "正在执行操作…", false, "", 0, true)
        else if (notificationBanner.working) notificationBanner.present("操作已完成", false, "", 2400)
    }
    property real guideScrollPosition: 0
    // Compact defaults derived from the real content: the ZeroTier table uses
    // 710 px of columns plus row padding, and the settings sections fit below
    // the default height without hiding their primary buttons.
    readonly property int defaultWindowWidth: 960
    readonly property int defaultWindowHeight: 700
    readonly property int minimumAppWidth: 640
    readonly property int minimumAppHeight: 440
    // Fixed sidebar widths. The old draggable window/qml/sidebarWidth key is
    // intentionally no longer read; only the explicit collapsed flag persists.
    readonly property int sidebarExpandedWidth: 184
    readonly property int sidebarCollapsedWidth: 56
    readonly property int availableWorkWidth: Screen.desktopAvailableWidth
    readonly property int availableWorkHeight: Screen.desktopAvailableHeight
    // Host settings live in one INI owned by SettingsStore; QML persistence goes
    // through the backend so the shared-root migration gate covers it too.
    property int savedWidth: Number(backend.hostSetting("window/qml/savedWidth", defaultWindowWidth))
    property int savedHeight: Number(backend.hostSetting("window/qml/savedHeight", defaultWindowHeight))
    property bool sidebarCollapsed: backend.hostSetting("window/qml/sidebarCollapsed", false) === true
    property real sidebarWidth: sidebarCollapsed ? sidebarCollapsedWidth : sidebarExpandedWidth
    Behavior on sidebarWidth { NumberAnimation { duration: 180; easing.type: Easing.InOutCubic } }
    function clampWindowSize(value, minimum, available) {
        const candidate = Math.max(minimum, Math.round(Number(value) || 0))
        if (!(Number(available) > 0)) return candidate
        return Math.max(minimum, Math.min(candidate, Number(available)))
    }
    function setSidebarCollapsed(collapsed) {
        if (sidebarCollapsed === collapsed) return
        sidebarCollapsed = collapsed
        backend.setHostSetting("window/qml/sidebarCollapsed", collapsed)
    }
    width: savedWidth
    height: savedHeight
    minimumWidth: minimumAppWidth
    minimumHeight: minimumAppHeight
    visible: true
    title: "Z2"
    // Windows draws its own caption buttons in the title bar below; other
    // platforms keep the native frame (macOS traffic lights stay native).
    readonly property bool customFrame: Qt.platform.os === "windows"
    readonly property bool maximized: visibility === Window.Maximized || visibility === Window.FullScreen
    readonly property int titleBarHeight: 48
    flags: customFrame
        ? Qt.Window | Qt.FramelessWindowHint | Qt.WindowSystemMenuHint | Qt.WindowMinimizeButtonHint | Qt.WindowMaximizeButtonHint
        : Qt.Window
    function toggleMaximized() { if (maximized) showNormal(); else showMaximized() }
    function openGuide() {
        // The guide points at ZeroTier page controls, so show that page first.
        if (backend.activeMenu !== "builtin/zerotier") backend.navigate("builtin/zerotier", pluginContainer)
        Qt.callLater(() => guideLoader.open())
    }
    color: Theme.window
    palette.window: Theme.window
    palette.windowText: Theme.text
    palette.base: Theme.field
    palette.text: Theme.text
    palette.button: Theme.button
    palette.buttonText: Theme.text
    palette.highlight: Theme.selection
    palette.highlightedText: Theme.text
    palette.mid: Theme.border
    palette.placeholderText: Theme.muted
    palette.midlight: Theme.hover
    palette.light: Theme.button
    palette.dark: Theme.border
    palette.alternateBase: Theme.side
    // Closing the window keeps ZTOrbit running in the tray; quitting is an
    // explicit choice (tray menu or Settings > 退出程序).
    onClosing: close => { close.accepted = false; if (backend.closing) backend.requestQuit(); else backend.hideToTray() }
    property bool trayHintShown: false
    Component.onCompleted: {
        // A saved size from a larger display must still open fully visible.
        width = clampWindowSize(savedWidth, minimumWidth, availableWorkWidth)
        height = clampWindowSize(savedHeight, minimumHeight, availableWorkHeight)
        Theme.dark = Qt.binding(() => backend.dark)
    }
    Component.onDestruction: {
        if (visibility !== Window.Maximized && visibility !== Window.Minimized) {
            backend.setHostSetting("window/qml/savedWidth", Math.max(minimumWidth, width))
            backend.setHostSetting("window/qml/savedHeight", Math.max(minimumHeight, height))
        }
    }
    property string pendingMenu: ""
    function navigateTo(menuId) {
        if (menuId === backend.activeMenu || backend.closing) return
        pendingMenu = menuId
        menuTransition.restart()
    }
    SequentialAnimation {
        id: menuTransition
        NumberAnimation { target: pageHost; property: "opacity"; to: 0; duration: 90; easing.type: Easing.InCubic }
        ScriptAction { script: backend.navigate(window.pendingMenu, pluginContainer) }
        NumberAnimation { target: pageHost; property: "opacity"; to: 1; duration: 140; easing.type: Easing.OutCubic }
    }
    NumberAnimation { id: pageReveal; target: pageHost; property: "opacity"; from: 0; to: 1; duration: 140 }
    function pluginStatus(value) {
        const labels = {"Ready": "已就绪", "Loading": "加载中…", "Preparing": "准备中…", "Preparing service": "服务准备中…", "Verifying": "校验中…", "Waiting for dependencies": "等待依赖…", "Starting worker": "服务启动中…", "Waiting for dependents": "等待依赖者停止…", "Stopping": "正在卸载…", "Disabled": "已禁用", "Not loaded": "未加载"}
        return labels[value] || value || "已加载"
    }
    function showDetails(kind, id, name) {
        detailKind = kind; detailId = id
        detailLoader.ensure().title = "详细信息 — " + name
        detailLoader.open()
    }
    function exportMoon(id) { exportNetworkId = id; exportLoader.ensure().currentFile = "file:///" + id + ".z2bundle"; exportLoader.open() }

    Platform.SystemTrayIcon {
        id: tray
        objectName: "systemTrayIcon"
        visible: true
        tooltip: backend.closing ? "Z2 · 正在退出" : backend.service.available === true ? "Z2 · ZeroTier 在线" : "Z2 · ZeroTier 离线"
        icon.source: backend.service.available === true ? "qrc:/qt/qml/ZTOrbit/assets/logo-online.png" : "qrc:/qt/qml/ZTOrbit/assets/logo-offline.png"
        onActivated: reason => { if (reason === Platform.SystemTrayIcon.Trigger || reason === Platform.SystemTrayIcon.DoubleClick) backend.restoreWindow() }
        menu: Platform.Menu {
            Platform.MenuItem { text: "显示窗口"; enabled: !backend.closing; onTriggered: backend.restoreWindow() }
            Platform.MenuItem { text: "退出"; enabled: !backend.closing; onTriggered: backend.requestQuit() }
        }
    }
    Connections {
        target: backend
        function onActiveMenuChanged() { if (!menuTransition.running) pageReveal.restart() }
        function onTrayRequested() {
            if (!tray.available) { window.showMinimized(); return }
            window.hide()
            if (!window.trayHintShown && tray.supportsMessages) {
                window.trayHintShown = true
                tray.showMessage("Z2 仍在后台运行", "点击托盘图标可重新打开窗口；如需退出，请在托盘菜单中选择“退出”。")
            }
        }
        function onNodeIdCopied() { notificationBanner.present("已复制", false, "", 1300) }
        function onNetworkJoined() { joinLoader.close() }
        function onConfigurationImported(networkId) { joinLoader.ensure().setNetworkId(networkId) }
        function onNotificationRequested(message, error) {
            notificationBanner.present(message, error, "", 0, window.operationPending && !error)
        }
        function onPluginNoticeRequested(title, message, error) {
            notificationBanner.present(message, error, title)
        }
        function onStateChanged() {
            if (backend.closing && notificationBanner.working) notificationBanner.message = backend.shutdownMessage
        }
        function onConfirmationRequested(title, message, acceptLabel) {
            const popup = confirmationLoader.ensure()
            popup.title = title; popup.message = message; popup.acceptLabel = acceptLabel; confirmationLoader.open()
        }
    }
    NotificationBanner {
        id: notificationBanner
        objectName: "notificationBanner"
        parent: Overlay.overlay
        anchors.top: parent.top
        anchors.topMargin: 18
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(560, parent.width - 32)
        maximumHeight: Math.min(260, parent.height - 48)
        z: 1000
    }
    // Title bar: the Z logo (service state + sidebar toggle) stays at a fixed
    // spot on the left, the window can be dragged by any empty area, and on
    // Windows the caption buttons sit flush in the top-right corner.
    Item {
        id: titleBar
        objectName: "titleBar"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: window.titleBarHeight
        enabled: !backend.closing
        DragHandler {
            target: null
            enabled: window.customFrame
            grabPermissions: PointerHandler.CanTakeOverFromAnything
            onActiveChanged: if (active) window.startSystemMove()
        }
        TapHandler {
            enabled: window.customFrame
            onDoubleTapped: window.toggleMaximized()
        }
        AbstractButton {
            id: logoButton
            objectName: "sidebarToggleButton"
            readonly property bool online: backend.service.available === true
            readonly property string stateText: online ? "ZeroTier 在线" : "ZeroTier 离线"
            // Centred over the collapsed sidebar, so it never moves.
            x: 12 + (window.sidebarCollapsedWidth - width) / 2
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: 34
            implicitHeight: 34
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: window.sidebarCollapsed ? "展开侧边栏" : "收起侧边栏"
            Accessible.description: stateText
            onClicked: window.setSidebarCollapsed(!window.sidebarCollapsed)
            contentItem: Item {
                Image {
                    objectName: "serviceStatusLogo"
                    anchors.centerIn: parent
                    width: logoButton.down ? 28 : 30
                    height: width
                    source: logoButton.online ? "qrc:/qt/qml/ZTOrbit/assets/logo-online.png" : "qrc:/qt/qml/ZTOrbit/assets/logo-offline.png"
                    sourceSize.width: 64
                    sourceSize.height: 64
                    smooth: true
                    mipmap: true
                    Behavior on width { NumberAnimation { duration: 80 } }
                }
            }
            background: Rectangle {
                radius: 9
                color: logoButton.hovered ? Theme.hover : "transparent"
                border.width: logoButton.activeFocus ? 2 : 0
                border.color: Theme.accent
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            AppToolTip {
                objectName: "sidebarToggleTooltip"
                parent: logoButton
                visible: logoButton.hovered
                text: logoButton.stateText + " · " + (window.sidebarCollapsed ? "点击展开侧边栏" : "点击收起侧边栏")
            }
        }
        Label {
            id: brandLabel
            objectName: "sidebarBrandLabel"
            text: "ZTOrbit"
            font.pixelSize: 22
            font.bold: true
            color: Theme.accent
            anchors.left: logoButton.right
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
        }
        AbstractButton {
            id: guideButton
            objectName: "beginnerGuideButton"
            anchors.left: brandLabel.right
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            implicitHeight: 26
            implicitWidth: guideRow.implicitWidth + 20
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            Accessible.role: Accessible.Button
            Accessible.name: "新手指南"
            onClicked: window.openGuide()
            contentItem: Item {
                Row {
                    id: guideRow
                    anchors.centerIn: parent
                    spacing: 6
                    Rectangle {
                        width: 16; height: 16; radius: 8
                        anchors.verticalCenter: parent.verticalCenter
                        color: Theme.primary
                        Text { anchors.centerIn: parent; text: "?"; color: Theme.primaryText; font.pixelSize: 11; font.bold: true }
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "新手指南"
                        font.pixelSize: Theme.fontCaption
                        color: guideButton.hovered ? Theme.accent : Theme.muted
                    }
                }
            }
            background: Rectangle {
                radius: height / 2
                color: guideButton.down ? Theme.selection : guideButton.hovered ? Theme.hover : "transparent"
                border.color: guideButton.activeFocus || guideButton.hovered ? Theme.accent : Theme.cardBorder
                border.width: guideButton.activeFocus ? 2 : 1
                Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 100 } }
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }
        Row {
            objectName: "windowButtons"
            visible: window.customFrame
            anchors.top: parent.top
            anchors.right: parent.right
            WindowButton { objectName: "minimizeWindowButton"; kind: "minimize"; Accessible.name: "最小化"; onClicked: window.showMinimized() }
            WindowButton {
                objectName: "maximizeWindowButton"
                kind: window.maximized ? "restore" : "maximize"
                Accessible.name: window.maximized ? "还原" : "最大化"
                onClicked: window.toggleMaximized()
            }
            WindowButton { objectName: "closeWindowButton"; kind: "close"; Accessible.name: "关闭"; onClicked: backend.hideToTray() }
        }
    }
    Item {
        id: workspace
        objectName: "workspace"
        anchors.fill: parent
        anchors.margins: 12
        anchors.topMargin: window.titleBarHeight
        enabled: !backend.closing
        Rectangle {
            id: sidebar
            objectName: "sidebar"
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: window.sidebarWidth
            radius: 6
            color: Theme.side
            clip: true
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: window.sidebarCollapsed ? 8 : 12
                spacing: 16
                ListView {
                    id: navigation
                    objectName: "navigation"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    model: backend.menus
                    spacing: 5
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    Accessible.name: "导航菜单"
                    delegate: ItemDelegate {
                        id: navigationItem
                        required property var modelData
                        objectName: "navigation-" + modelData.id
                        width: navigation.width
                        height: 44
                        text: modelData.title
                        enabled: modelData.ready
                        highlighted: backend.activeMenu === modelData.id
                        hoverEnabled: true
                        focusPolicy: Qt.StrongFocus
                        activeFocusOnTab: true
                        padding: 0
                        readonly property string title: modelData.title || ""
                        readonly property string iconSource: modelData.iconSource || ""
                        readonly property bool loading: modelData.loading || false
                        readonly property bool hasIcon: iconSource.length > 0
                        readonly property bool usesZeroTierIcon: !hasIcon && !loading && modelData.iconName === "zerotier"
                        readonly property bool usesFallbackIcon: !hasIcon && !loading && modelData.iconName !== "zerotier"
                        readonly property string tooltipText: window.sidebarCollapsed ? title : (modelData.message || modelData.toolTip || "")
                        readonly property string accessibleName: title
                        Accessible.name: navigationItem.accessibleName
                        Accessible.description: modelData.message || modelData.toolTip || ""
                        onClicked: window.navigateTo(modelData.id)
                        contentItem: Item {
                            implicitWidth: 24
                            implicitHeight: 24
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8
                                Item {
                                    implicitWidth: 24
                                    implicitHeight: 24
                                    Image {
                                        objectName: "navigationIcon-" + navigationItem.modelData.id
                                        visible: navigationItem.hasIcon
                                        anchors.centerIn: parent
                                        width: 20; height: 20
                                        source: navigationItem.iconSource
                                        sourceSize.width: 20; sourceSize.height: 20
                                        fillMode: Image.PreserveAspectFit
                                        opacity: navigationItem.enabled ? 1 : 0.45
                                    }
                                    ZeroTierIcon {
                                        visible: navigationItem.usesZeroTierIcon
                                        anchors.centerIn: parent
                                        width: 20; height: 20
                                        ink: navigationItem.enabled ? Theme.text : Theme.muted
                                        opacity: navigationItem.enabled ? 1 : 0.55
                                    }
                                    PluginIcon {
                                        objectName: "navigationFallbackIcon-" + navigationItem.modelData.id
                                        visible: navigationItem.usesFallbackIcon
                                        anchors.centerIn: parent
                                        width: 20; height: 20
                                        ink: navigationItem.enabled ? Theme.text : Theme.muted
                                        opacity: navigationItem.enabled ? 1 : 0.55
                                    }
                                    BusyIndicator {
                                        objectName: "navigationBusy-" + navigationItem.modelData.id
                                        anchors.centerIn: parent
                                        width: 20; height: 20
                                        running: navigationItem.loading
                                        visible: running
                                    }
                                }
                                Label {
                                    id: navigationLabel
                                    objectName: "navigationLabel-" + navigationItem.modelData.id
                                    text: navigationItem.title + (navigationItem.loading ? " · 加载中…" : "")
                                    elide: Text.ElideRight
                                    color: navigationItem.enabled ? Theme.text : Theme.muted
                                    font.pixelSize: 16
                                    horizontalAlignment: Text.AlignLeft
                                    verticalAlignment: Text.AlignVCenter
                                    Layout.fillWidth: true
                                    opacity: window.sidebarCollapsed ? 0 : 1
                                    visible: opacity > 0.01
                                    Behavior on opacity { NumberAnimation { duration: 120 } }
                                }
                            }
                        }
                        background: Rectangle {
                            radius: 5
                            color: navigationItem.highlighted ? Theme.selection : navigationItem.hovered ? Theme.hover : "transparent"
                            border.width: navigationItem.activeFocus ? 2 : 0
                            border.color: Theme.accent
                            Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 100 } }
                            Rectangle {
                                visible: parent.parent.highlighted
                                width: 3; height: 22
                                anchors.verticalCenter: parent.verticalCenter
                                color: Theme.accent
                            }
                        }
                        AppToolTip {
                            objectName: "navigationTooltip-" + navigationItem.modelData.id
                            parent: navigationItem
                            visible: navigationItem.hovered && navigationItem.tooltipText !== ""
                            text: navigationItem.tooltipText
                        }
                    }
                }
                Item {
                    id: sidebarFooter
                    objectName: "sidebarFooter"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    GearButton {
                        id: settingsButton
                        objectName: "settingsButton"
                        x: window.sidebarCollapsed ? (sidebarFooter.width - width) / 2 : 0
                        selected: backend.activeMenu === "builtin/settings"
                        onClicked: window.navigateTo("builtin/settings")
                        Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.InOutCubic } }
                    }
                    Label {
                        objectName: "sidebarVersionLabel"
                        text: "v" + backend.version
                        font.pixelSize: Theme.fontCaption
                        color: Theme.muted
                        anchors.left: settingsButton.right
                        anchors.leftMargin: 10
                        anchors.verticalCenter: settingsButton.verticalCenter
                        opacity: window.sidebarCollapsed ? 0 : 1
                        visible: opacity > 0.01
                        Behavior on opacity { NumberAnimation { duration: 120 } }
                    }
                }
            }
        }
        Item {
            id: pageHost
            anchors.left: sidebar.right
            anchors.leftMargin: 10
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            BoundedScrollView {
                id: scroll
                objectName: "zeroTierScroll"
                anchors.fill: parent
                visible: backend.activeMenu === "builtin/zerotier"
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ColumnLayout {
                    width: scroll.availableWidth
                    spacing: 16
                    // Page header: title on the left, help on the right.
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        Label { text: "ZeroTier"; color: Theme.text; font.pixelSize: Theme.fontTitle; font.bold: true; Layout.fillWidth: true }
                    }
                    // This device: service state, node identity and the service
                    // controls live together, because they describe one thing.
                    Card {
                        id: nodeCard
                        objectName: "nodeCard"
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        readonly property color stateColor: !backend.service.known ? Theme.muted
                            : backend.service.available ? Theme.online
                            : backend.service.running ? Theme.warning : Theme.offline
                        readonly property string stateText: !backend.service.known ? "正在检测 ZeroTier 服务…"
                            : !backend.service.exists ? "ZeroTier 未安装"
                            : backend.service.available ? "服务运行中"
                            : backend.service.running ? "服务已启动，等待响应…" : "服务已停止"
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                RowLayout {
                                    spacing: 8
                                    Rectangle { implicitWidth: 8; implicitHeight: 8; radius: 4; color: nodeCard.stateColor }
                                    Label { objectName: "serviceStateLabel"; text: nodeCard.stateText; color: Theme.muted; font.pixelSize: Theme.fontCaption }
                                }
                                Label {
                                    id: nodeId
                                    objectName: "nodeId"
                                    text: backend.info.address || "—"
                                    color: nodeHover.hovered && validId ? Theme.accent : Theme.text
                                    readonly property bool validId: /^[0-9a-fA-F]{10}$/.test(text)
                                    font.underline: nodeHover.hovered && validId
                                    font.pixelSize: 26
                                    font.bold: true
                                    font.letterSpacing: 0.5
                                    HoverHandler { id: nodeHover; cursorShape: nodeId.validId ? Qt.PointingHandCursor : Qt.ArrowCursor }
                                    TapHandler { onTapped: backend.copyNodeId() }
                                    TapHandler { acceptedButtons: Qt.RightButton; onTapped: nodeMenu.popup() }
                                    AppMenu { id: nodeMenu; AppMenuItem { text: "完整节点信息"; onTriggered: nodeInfoLoader.open() } }
                                    AppToolTip { parent: nodeId; visible: nodeHover.hovered && nodeId.validId; delay: 600; text: "单击复制节点 ID，右键查看完整信息" }
                                }
                                Label {
                                    text: "本机节点 ID" + (backend.info.version ? " · ZeroTier v" + backend.info.version : " · 节点信息尚未读取")
                                    color: Theme.muted
                                    font.pixelSize: Theme.fontCaption
                                }
                            }
                            // Split button: the main service action plus a menu
                            // for the rarer restart / uninstall.
                            Rectangle {
                                Layout.alignment: Qt.AlignVCenter
                                implicitWidth: serviceActions.implicitWidth
                                implicitHeight: 34
                                radius: Theme.radius
                                color: Theme.button
                                border.color: Theme.border
                                RowLayout {
                                    id: serviceActions
                                    anchors.fill: parent
                                    spacing: 0
                                    ActionButton {
                                        id: serviceButton
                                        objectName: "serviceAction"
                                        text: backend.service.action
                                        flat: true
                                        Layout.fillHeight: true
                                        enabled: !backend.busy && backend.service.known
                                        onClicked: { if (!backend.service.exists) backend.packageAction(false); else backend.serviceAction(backend.service.running ? "stop" : "start") }
                                    }
                                    Rectangle { visible: backend.service.exists; implicitWidth: 1; Layout.preferredHeight: 18; color: Theme.border }
                                    ActionButton {
                                        id: serviceMoreButton
                                        objectName: "serviceMore"
                                        flat: true
                                        implicitWidth: 34
                                        horizontalPadding: 0
                                        Layout.fillHeight: true
                                        visible: backend.service.exists
                                        enabled: !backend.busy && backend.service.known
                                        Accessible.name: "更多服务操作"
                                        contentItem: Item { ChevronIcon { anchors.centerIn: parent; width: 16; height: 16; pointingRight: true; rotation: 90; ink: serviceMoreButton.enabled ? Theme.text : Theme.muted } }
                                        onClicked: serviceMenu.open()
                                        AppMenu {
                                            id: serviceMenu
                                            y: parent.height + 5
                                            x: parent.width - width
                                            AppMenuItem { text: "重启服务"; visible: backend.service.running; onTriggered: backend.serviceAction("restart") }
                                            AppMenuItem { text: "卸载 ZeroTier"; onTriggered: backend.packageAction(true) }
                                        }
                                    }
                                }
                            }
                        }
                        Label { text: backend.service.warning; visible: text !== ""; color: Theme.danger; wrapMode: Text.Wrap; font.pixelSize: Theme.fontCaption; Layout.fillWidth: true }
                    }
                    // Networks: the section's actions sit in its own header, so
                    // "join" is found where the list is, even when it is empty.
                    Card {
                        objectName: "networksCard"
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        padding: 0
                        spacing: 0
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.margins: 16
                            Layout.bottomMargin: 12
                            spacing: 8
                            Label { text: "网络"; font.pixelSize: Theme.fontHeading; font.bold: true; color: Theme.text }
                            Label { text: backend.networks.count > 0 ? String(backend.networks.count) : ""; visible: text !== ""; color: Theme.muted; font.pixelSize: Theme.fontCaption }
                            Item { Layout.fillWidth: true }
                            ActionButton { id: moonButton; objectName: "moonsButton"; text: "中转站"; flat: true; onClicked: moonLoader.open() }
                            ActionButton { id: joinButton; objectName: "joinNetworkButton"; text: "加入网络"; primary: true; enabled: !backend.busy; onClicked: { joinLoader.ensure().setNetworkId(""); joinLoader.open() } }
                        }
                        RecordList {
                            id: networks
                            objectName: "networks"
                            Layout.fillWidth: true
                            Layout.bottomMargin: 6
                            backend: window.backend
                            records: backend.networks
                            kind: "networks"
                            headers: ["名称", "网络 ID", "状态", "分配的 IP"]
                            statusText: backend.networkStatus
                            onDetailsRequested: (kind, rowId, name) => window.showDetails(kind, rowId, name)
                            onExportRequested: rowId => window.exportMoon(rowId)
                        }
                    }
                    // Members: collapsible, the whole header toggles it.
                    Card {
                        objectName: "peersCard"
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        padding: 0
                        spacing: 0
                        Item {
                            Layout.fillWidth: true
                            implicitHeight: peersHeader.implicitHeight + 24
                            HoverHandler { id: peersHeaderHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler {
                                // The chevron button handles its own clicks; never toggle twice.
                                onTapped: (eventPoint) => {
                                    const local = togglePeersButton.mapFromItem(parent, eventPoint.position.x, eventPoint.position.y)
                                    if (togglePeersButton.contains(local)) return
                                    backend.peersExpanded = !backend.peersExpanded
                                }
                            }
                            RowLayout {
                                id: peersHeader
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 10
                                anchors.rightMargin: 16
                                spacing: 4
                                ToolButton {
                                    id: togglePeersButton
                                    objectName: "togglePeersButton"
                                    implicitWidth: 28
                                    implicitHeight: 28
                                    focusPolicy: Qt.TabFocus
                                    Accessible.name: backend.peersExpanded ? "收起成员" : "展开成员"
                                    onClicked: backend.peersExpanded = !backend.peersExpanded
                                    contentItem: Item {
                                        ChevronIcon {
                                            anchors.centerIn: parent
                                            width: 16; height: 16
                                            pointingRight: true
                                            rotation: backend.peersExpanded ? 90 : 0
                                            ink: peersHeaderHover.hovered ? Theme.accent : Theme.text
                                            Behavior on rotation { NumberAnimation { duration: 160; easing.type: Easing.InOutCubic } }
                                        }
                                    }
                                    background: Rectangle { radius: Theme.radius; color: togglePeersButton.hovered ? Theme.hover : "transparent"; border.color: togglePeersButton.activeFocus ? Theme.accent : "transparent" }
                                }
                                Label { id: peersHeading; text: "成员"; font.pixelSize: Theme.fontHeading; font.bold: true; color: Theme.text }
                                Label { text: backend.peers.count > 0 ? String(backend.peers.count) : ""; visible: text !== ""; color: Theme.muted; font.pixelSize: Theme.fontCaption; Layout.leftMargin: 4 }
                                Item { Layout.fillWidth: true }
                            }
                        }
                        Item {
                            Layout.fillWidth: true
                            implicitHeight: backend.peersExpanded ? peers.implicitHeight + 6 : 0
                            opacity: backend.peersExpanded ? 1 : 0
                            visible: implicitHeight > 0
                            clip: true
                            Behavior on implicitHeight { NumberAnimation { duration: 180; easing.type: Easing.InOutCubic } }
                            Behavior on opacity { NumberAnimation { duration: 140 } }
                            RecordList {
                                id: peers
                                objectName: "peers"
                                width: parent.width
                                backend: window.backend
                                records: backend.peers
                                kind: "peers"
                                headers: ["名称", "角色", "连接", "IP 地址", "延迟（ms）"]
                                statusText: backend.peerStatus
                                onDetailsRequested: (kind, rowId, name) => window.showDetails(kind, rowId, name)
                            }
                        }
                    }
                    Item { Layout.preferredHeight: 16 }
                }
            }
            SettingsPage {
                anchors.fill: parent
                visible: backend.activeMenu === "builtin/settings"
                backend: window.backend
                onOpenPlugins: pluginLoader.open()
                onOpenAbout: aboutLoader.open()
            }
            Item { id: pluginContainer; objectName: "pluginContainer"; anchors.fill: parent; visible: backend.activeMenu !== "builtin/zerotier" && backend.activeMenu !== "builtin/settings" }
        }
    }
    LazyPopup {
        id: joinLoader
        sourceComponent: AppDialog {
            id: joinDialog
            function setNetworkId(value) { networkIdInput.text = value }
            objectName: "joinNetworkDialog"
            title: "加入网络"
            anchors.centerIn: parent
            width: Math.min(460, window.width - 40)
            modal: true
            closePolicy: backend.busy ? Popup.NoAutoClose : Popup.CloseOnEscape
            onOpened: networkIdInput.forceActiveFocus()
            ColumnLayout {
                anchors.fill: parent
                spacing: 16
                TextField { id: networkIdInput; objectName: "networkIdInput"; placeholderText: "16 位网络 ID"; maximumLength: 16; Layout.fillWidth: true; enabled: !backend.busy; onAccepted: backend.joinNetwork(text) }
                Label { text: "填写管理员提供的网络 ID；加入后仍需管理员授权本机。也可以导入管理员提供的中转站配置包。"; color: Theme.muted; font.pixelSize: Theme.fontCaption; wrapMode: Text.Wrap; Layout.fillWidth: true }
                RowLayout {
                    ActionButton { objectName: "importMoonButton"; text: "导入中转站"; enabled: !backend.busy; onClicked: importBundleLoader.open() }
                    Item { Layout.fillWidth: true }
                    ActionButton { text: "取消"; enabled: !backend.busy; onClicked: joinLoader.close() }
                    ActionButton { objectName: "confirmJoinButton"; text: "确认加入"; primary: true; enabled: !backend.busy; onClicked: backend.joinNetwork(networkIdInput.text) }
                }
            }
        }
    }
    LazyPopup {
        id: confirmationLoader
        sourceComponent: AppDialog {
            id: confirmation
            objectName: "confirmationDialog"
            property string message: ""
            property string acceptLabel: "继续"
            anchors.centerIn: parent
            width: Math.min(480, window.width - 40)
            modal: true
            closePolicy: Popup.CloseOnEscape
            onRejected: backend.confirm(false)
            onAccepted: backend.confirm(true)
            ColumnLayout {
                anchors.fill: parent
                spacing: 20
                Label { text: confirmation.message; wrapMode: Text.Wrap; Layout.fillWidth: true }
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    ActionButton { objectName: "cancelActionButton"; text: "取消"; focus: true; onClicked: confirmation.reject() }
                    ActionButton { objectName: "confirmActionButton"; text: confirmation.acceptLabel; destructive: true; onClicked: confirmation.accept() }
                }
            }
        }
    }
    LazyPopup {
        id: moonLoader
        sourceComponent: AppDialog {
            id: moonDialog
            objectName: "moonDialog"
            title: "中转站"
            anchors.centerIn: parent
            width: Math.min(560, window.width - 40)
            height: Math.min(480, window.height - 60)
            modal: false
            onAboutToShow: backend.moonsActive = true
            onClosed: backend.moonsActive = false
            ColumnLayout {
                anchors.fill: parent
                BoundedScrollView {
                    id: moonScroll
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: availableWidth
                    RecordList { objectName: "moonRecords"; width: moonScroll.availableWidth; backend: window.backend; records: backend.moons; kind: "moons"; headers: ["名称"]; onDetailsRequested: (kind, rowId, name) => window.showDetails(kind, rowId, name) }
                }
                RowLayout {
                    ActionButton { objectName: "addMoonButton"; text: "添加 Moon 文件"; enabled: !backend.busy; onClicked: importMoonLoader.open() }
                    Item { Layout.fillWidth: true }
                    ActionButton { text: "关闭"; onClicked: moonLoader.close() }
                }
            }
            DropArea { anchors.fill: parent; onEntered: drag => { drag.accepted = drag.hasUrls && !backend.busy }; onDropped: drop => backend.importMoons(drop.urls) }
        }
    }
    LazyPopup {
        id: detailLoader
        sourceComponent: AppDialog {
            id: detailDialog
            objectName: "detailsPopup"
            anchors.centerIn: parent
            width: Math.min(680, window.width - 40)
            height: Math.min(500, window.height - 60)
            modal: false
            ColumnLayout {
                anchors.fill: parent
                TabBar {
                    id: detailTabs
                    Layout.fillWidth: true
                    TabButton { text: "完整信息"; background: Rectangle { color: parent.checked ? Theme.selection : Theme.popupHeader; radius: 5; Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 120 } } } }
                    TabButton { text: "原始 JSON"; background: Rectangle { color: parent.checked ? Theme.selection : Theme.popupHeader; radius: 5; Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 120 } } } }
                }
                StackLayout {
                    currentIndex: detailTabs.currentIndex
                    onCurrentIndexChanged: detailReveal.restart()
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    ListView {
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        model: backend.detailFields(window.detailKind, window.detailId)
                        ScrollBar.vertical: ScrollBar {}
                        delegate: RowLayout {
                            required property var modelData
                            width: ListView.view.width
                            Label { text: modelData.key; Layout.preferredWidth: 210; elide: Text.ElideRight }
                            Label { text: modelData.value; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        }
                    }
                    BoundedScrollView { TextArea { objectName: "detailsJson"; text: backend.detailsJson(window.detailKind, window.detailId); readOnly: true; selectByMouse: true; wrapMode: TextArea.Wrap } }
                }
                ActionButton { text: "关闭"; Layout.alignment: Qt.AlignRight; onClicked: detailLoader.close() }
                NumberAnimation { id: detailReveal; target: detailTabs.parent; property: "opacity"; from: 0.4; to: 1; duration: 130 }
            }
        }
    }
    LazyPopup {
        id: nodeInfoLoader
        sourceComponent: AppDialog {
            id: nodeInfo
            title: "完整节点信息"
            anchors.centerIn: parent
            width: Math.min(500, window.width - 40)
            height: Math.min(420, window.height - 60)
            standardButtons: Dialog.Close
            BoundedScrollView { anchors.fill: parent; TextArea { readOnly: true; selectByMouse: true; text: JSON.stringify(backend.info, null, 2); wrapMode: TextArea.Wrap } }
        }
    }
    LazyPopup {
        id: pluginLoader
        sourceComponent: AppDialog {
            id: pluginDialog
            objectName: "pluginDialog"
            property string selectedPath: ""
            title: "插件"
            anchors.centerIn: parent
            width: Math.min(520, window.width - 40)
            height: Math.min(430, window.height - 60)
            modal: false
            closePolicy: Popup.CloseOnEscape
            ColumnLayout {
                anchors.fill: parent
                Label { text: "可以运行时加载或卸载插件；卸载保留插件文件和数据。"; wrapMode: Text.Wrap; Layout.fillWidth: true; color: Theme.muted }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    model: backend.plugins
                    delegate: ItemDelegate {
                        id: pluginRow
                        required property var modelData
                        objectName: "pluginRow-" + modelData.id
                        width: ListView.view.width
                        text: modelData.name + (modelData.version ? "  v" + modelData.version : "") + "  " + window.pluginStatus(modelData.message)
                        highlighted: pluginDialog.selectedPath === modelData.path
                        onClicked: pluginDialog.selectedPath = modelData.path
                        contentItem: Text { text: pluginRow.text; color: Theme.text; font: pluginRow.font; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter }
                        background: Rectangle { radius: 4; color: pluginRow.highlighted ? Theme.selection : pluginRow.hovered ? Theme.hover : "transparent" }
                        TapHandler { acceptedButtons: Qt.RightButton; onTapped: (point) => { pluginDialog.selectedPath = modelData.path; pluginMenu.popup(pluginRow, point.position.x, point.position.y) } }
                        AppMenu {
                            id: pluginMenu
                            AppMenuItem { objectName: "pluginLoad-" + modelData.id; text: "加载"; visible: !modelData.loaded; enabled: modelData.loadable && !modelData.loading && !modelData.stopping; onTriggered: backend.loadPlugin(modelData.path) }
                            AppMenuItem { objectName: "pluginUnload-" + modelData.id; text: "卸载"; visible: modelData.loaded; enabled: !modelData.stopping; onTriggered: backend.unloadPlugin(modelData.id) }
                            AppMenuItem { objectName: "pluginDelete-" + modelData.id; text: "删除"; visible: !modelData.loaded; enabled: !modelData.loading && !modelData.stopping; onTriggered: backend.deletePlugin(modelData.path) }
                            AppMenuItem { objectName: "pluginAbout-" + modelData.id; text: "关于"; onTriggered: { window.aboutPlugin = modelData; pluginAboutLoader.open() } }
                        }
                    }
                    Label { anchors.centerIn: parent; visible: backend.plugins.length === 0; text: "没有已安装的插件"; color: Theme.muted }
                }
                RowLayout {
                    ActionButton { objectName: "importPluginButton"; text: "导入插件"; onClicked: pluginFileLoader.open() }
                    Item { Layout.fillWidth: true }
                    ActionButton { text: "关闭"; onClicked: pluginLoader.close() }
                }
            }
        }
    }
    LazyPopup {
        id: pluginAboutLoader
        sourceComponent: AppDialog {
            id: pluginAbout
            panelColor: Theme.popupHeader
            objectName: "pluginAboutDialog"
            title: "关于插件"
            anchors.centerIn: parent
            width: Math.min(430, window.width - 40)
            standardButtons: Dialog.Close
            ColumnLayout {
                anchors.fill: parent
                spacing: 12
                Label { text: window.aboutPlugin.name || ""; font.pixelSize: 20; font.bold: true }
                Label { text: "插件 ID：" + (window.aboutPlugin.id || ""); wrapMode: Text.Wrap; Layout.fillWidth: true }
                Label { text: "版本：" + (window.aboutPlugin.version || "") }
                Label { text: "API：" + (window.aboutPlugin.api || "") }
                Label { text: "运行方式：" + (window.aboutPlugin.execution === "worker-process" ? "独立服务进程" : "进程内插件") }
                Label { objectName: "pluginServicesLabel"; text: "提供服务：" + ((window.aboutPlugin.provides || []).map(s => s.service + " / " + s.api).join("、") || "无"); wrapMode: Text.Wrap; Layout.fillWidth: true }
                Label { text: "前置依赖：" + ((window.aboutPlugin.dependencies || []).map(d => d.pluginId + " " + d.versionRange).join("、") || "无"); wrapMode: Text.Wrap; Layout.fillWidth: true }
                Label { text: "使用者：" + ((window.aboutPlugin.usedBy || []).join("、") || "无"); wrapMode: Text.Wrap; Layout.fillWidth: true }
                Label { text: "加载状态：" + window.pluginStatus(window.aboutPlugin.message) }
            }
        }
    }
    LazyPopup {
        id: aboutLoader
        sourceComponent: AppDialog {
            id: aboutDialog
            objectName: "aboutDialog"
            title: "关于"
            anchors.centerIn: parent
            standardButtons: Dialog.Close
            ColumnLayout {
                anchors.fill: parent
                spacing: 12
                Label { text: "ZTOrbit"; font.pixelSize: 24; font.bold: true }
                Label { objectName: "aboutVersionLabel"; text: "v" + backend.version }
                Label { text: "API " + backend.apiVersion }
                Label { objectName: "aboutServerVersionLabel"; text: "服务端 " + backend.serverVersion + "（API " + backend.serverApiVersion + "）" }
                Label { text: "Qt " + backend.qtVersion }
            }
        }
    }
    // Frameless windows lose the system border: draw a hairline and keep the
    // edges and corners resizable through the window manager.
    Rectangle {
        anchors.fill: parent
        visible: window.customFrame && !window.maximized
        color: "transparent"
        border.color: Theme.border
        z: 900
    }
    Item {
        id: resizeEdges
        anchors.fill: parent
        visible: window.customFrame && !window.maximized
        z: 901
        readonly property int grip: 5
        Repeater {
            // Edges and corners; each grip hands the resize to the window manager.
            model: [Qt.LeftEdge, Qt.RightEdge, Qt.TopEdge, Qt.BottomEdge,
                    Qt.LeftEdge | Qt.TopEdge, Qt.RightEdge | Qt.TopEdge,
                    Qt.LeftEdge | Qt.BottomEdge, Qt.RightEdge | Qt.BottomEdge]
            delegate: MouseArea {
                required property int modelData
                readonly property bool atLeft: (modelData & Qt.LeftEdge) !== 0
                readonly property bool atRight: (modelData & Qt.RightEdge) !== 0
                readonly property bool atTop: (modelData & Qt.TopEdge) !== 0
                readonly property bool atBottom: (modelData & Qt.BottomEdge) !== 0
                readonly property bool corner: (atLeft || atRight) && (atTop || atBottom)
                readonly property int grip: resizeEdges.grip
                width: atLeft || atRight ? (corner ? grip * 2 : grip) : resizeEdges.width - grip * 2
                height: atTop || atBottom ? (corner ? grip * 2 : grip) : resizeEdges.height - grip * 2
                x: atLeft ? 0 : atRight ? resizeEdges.width - width : grip
                y: atTop ? 0 : atBottom ? resizeEdges.height - height : grip
                acceptedButtons: Qt.LeftButton
                cursorShape: !corner ? (atLeft || atRight ? Qt.SizeHorCursor : Qt.SizeVerCursor)
                    : (atLeft && atTop) || (atRight && atBottom) ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
                onPressed: window.startSystemResize(modelData)
            }
        }
    }
    LazyPopup {
        id: guideLoader
        popupParent: Overlay.overlay
        sourceComponent: Guide {
            id: guide
            objectName: "guideOverlay"
            parent: Overlay.overlay
            targets: [serviceButton, nodeId, joinButton, networks, peersHeading, moonButton]
            onAboutToShow: window.guideScrollPosition = scroll.contentItem.contentY
            onClosed: scroll.contentItem.contentY = window.guideScrollPosition
            onTargetRequested: target => {
                if (!target) return
                const point = target.mapToItem(scroll.contentItem.contentItem, 0, 0)
                scroll.contentItem.contentY = Math.max(0, Math.min(scroll.contentItem.contentHeight - scroll.contentItem.height, point.y - 80))
            }
        }
    }
    LazyPopup {
        id: importBundleLoader
        sourceComponent: FileDialog { id: importBundleFile; title: "导入中转站"; nameFilters: ["中转站配置 (*.z2bundle *.moon)"]; onAccepted: backend.importConfiguration(selectedFile) }

    }
    LazyPopup {
        id: importMoonLoader
        sourceComponent: FileDialog { id: importMoonFile; title: "选择 Moon 文件"; fileMode: FileDialog.OpenFiles; nameFilters: ["Moon (*.moon)"]; onAccepted: backend.importMoons(selectedFiles) }

    }
    LazyPopup {
        id: exportLoader
        sourceComponent: FileDialog { id: exportFile; title: "导出中转站"; fileMode: FileDialog.SaveFile; nameFilters: ["中转站配置包 (*.z2bundle)"]; onAccepted: backend.exportConfiguration(window.exportNetworkId, selectedFile) }

    }
    LazyPopup {
        id: pluginFileLoader
        sourceComponent: FileDialog { id: pluginFile; title: "导入插件"; nameFilters: ["插件 (*.z2plugin)"]; onAccepted: backend.importPlugin(selectedFile) }

    }
}
