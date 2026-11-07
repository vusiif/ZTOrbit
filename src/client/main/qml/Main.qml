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
    onClosing: close => { close.accepted = false; backend.requestQuit() }
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
        tooltip: backend.closing ? "ZTOrbit · 正在退出" : "ZTOrbit"
        icon.source: "qrc:/assets/app-icon.png"
        onActivated: reason => { if (reason === Platform.SystemTrayIcon.Trigger || reason === Platform.SystemTrayIcon.DoubleClick) backend.restoreWindow() }
        menu: Platform.Menu {
            Platform.MenuItem { text: "显示窗口"; enabled: !backend.closing; onTriggered: backend.restoreWindow() }
            Platform.MenuItem { text: "退出"; enabled: !backend.closing; onTriggered: backend.requestQuit() }
        }
    }
    Connections {
        target: backend
        function onActiveMenuChanged() { if (!menuTransition.running) pageReveal.restart() }
        function onTrayRequested() { if (tray.available) window.hide(); else window.showMinimized() }
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
    Item {
        id: workspace
        objectName: "workspace"
        anchors.fill: parent
        anchors.margins: 12
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
                Item {
                    id: sidebarHeader
                    objectName: "sidebarHeader"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    Rectangle {
                        objectName: "serviceStatusDot"
                        implicitWidth: 10; implicitHeight: 10; radius: 5
                        x: window.sidebarCollapsed ? (sidebarHeader.width - width) / 2 : 0
                        y: (sidebarHeader.height - height) / 2
                        color: backend.service.available ? "#25a773" : "#d95059"
                        Accessible.name: backend.service.available ? "ZeroTier 在线" : "ZeroTier 离线"
                        ToolTip.visible: statusHover.hovered
                        ToolTip.text: backend.service.available ? "ZeroTier 在线" : "ZeroTier 离线"
                        HoverHandler { id: statusHover }
                        Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.InOutCubic } }
                        Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 140 } }
                    }
                    Label {
                        objectName: "sidebarBrandLabel"
                        text: "ZTOrbit"
                        font.pixelSize: 24
                        font.bold: true
                        color: Theme.accent
                        x: 18
                        y: (sidebarHeader.height - height) / 2
                        opacity: window.sidebarCollapsed ? 0 : 1
                        visible: opacity > 0.01
                        Behavior on opacity { NumberAnimation { duration: 120 } }
                    }
                    Label {
                        objectName: "sidebarVersionLabel"
                        text: "v" + backend.version
                        font.pixelSize: 11
                        color: Theme.muted
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        opacity: window.sidebarCollapsed ? 0 : 1
                        visible: opacity > 0.01
                        Behavior on opacity { NumberAnimation { duration: 120 } }
                    }
                }
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
                    Layout.preferredHeight: window.sidebarCollapsed ? 92 : 40
                    GearButton {
                        id: settingsButton
                        objectName: "settingsButton"
                        x: 0
                        y: window.sidebarCollapsed ? 52 : 0
                        selected: backend.activeMenu === "builtin/settings"
                        onClicked: window.navigateTo("builtin/settings")
                        Behavior on y { NumberAnimation { duration: 180; easing.type: Easing.InOutCubic } }
                    }
                    SidebarToggleButton {
                        id: sidebarToggle
                        collapsed: window.sidebarCollapsed
                        x: window.sidebarCollapsed ? (sidebarFooter.width - width) / 2 : sidebarFooter.width - width
                        y: 0
                        Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.InOutCubic } }
                        onToggleRequested: window.setSidebarCollapsed(!window.sidebarCollapsed)
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
                    spacing: 20
                    Item { Layout.preferredHeight: 2 }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        Label { text: "ZeroTier"; color: Theme.text; font.pixelSize: 26; font.bold: true; Layout.fillWidth: true }
                        ActionButton { objectName: "beginnerGuideButton"; text: "新手指南"; onClicked: guideLoader.open() }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        ColumnLayout {
                            Layout.fillWidth: true
                            Label {
                                id: nodeId
                                objectName: "nodeId"
                                text: backend.info.address || "—"
                                color: nodeHover.hovered && validId ? Theme.accent : Theme.text
                                readonly property bool validId: /^[0-9a-fA-F]{10}$/.test(text)
                                font.underline: nodeHover.hovered && validId
                                HoverHandler { id: nodeHover; cursorShape: nodeId.validId ? Qt.PointingHandCursor : Qt.ArrowCursor }
                                font.pixelSize: 28
                                font.bold: true
                                TapHandler { onTapped: backend.copyNodeId() }
                                TapHandler { acceptedButtons: Qt.RightButton; onTapped: nodeMenu.popup() }
                                AppMenu { id: nodeMenu; AppMenuItem { text: "完整节点信息"; onTriggered: nodeInfoLoader.open() } }
                            }
                            Label { text: backend.info.version ? "v" + backend.info.version : "节点信息尚未读取"; color: Theme.muted }
                        }
                        Rectangle {
                            implicitWidth: serviceActions.implicitWidth
                            implicitHeight: 36
                            radius: 5
                            color: Theme.selection
                            border.color: Theme.border
                            clip: true
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
                                Rectangle { visible: backend.service.exists; implicitWidth: 1; Layout.preferredHeight: 20; color: Theme.border }
                                ActionButton {
                                    objectName: "serviceMore"
                                    text: "▾"
                                    flat: true
                                    horizontalPadding: 9
                                    visible: backend.service.exists
                                    enabled: !backend.busy && backend.service.known
                                    Accessible.name: "更多服务操作"
                                    onClicked: serviceMenu.open()
                                    AppMenu {
                                        id: serviceMenu
                                        y: parent.height + 5
                                        x: parent.width - width
                                        AppMenuItem { text: "重启"; visible: backend.service.running; onTriggered: backend.serviceAction("restart") }
                                        AppMenuItem { text: "卸载"; onTriggered: backend.packageAction(true) }
                                    }
                                }
                            }
                        }
                    }
                    Label { text: backend.service.warning; visible: text !== ""; color: Theme.danger; wrapMode: Text.Wrap; Layout.fillWidth: true; Layout.leftMargin: 16; Layout.rightMargin: 16 }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        Label { text: "网络"; font.pixelSize: 18; font.bold: true; color: Theme.text; Layout.fillWidth: true }
                    }
                    RecordList {
                        id: networks
                        objectName: "networks"
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        backend: window.backend
                        records: backend.networks
                        kind: "networks"
                        headers: ["名称", "网络 ID", "状态", "分配的 IP"]
                        statusText: backend.networkStatus
                        onDetailsRequested: (kind, rowId, name) => window.showDetails(kind, rowId, name)
                        onExportRequested: rowId => window.exportMoon(rowId)
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        ActionButton { id: joinButton; objectName: "joinNetworkButton"; text: "加入网络"; primary: true; enabled: !backend.busy; onClicked: { joinLoader.ensure().setNetworkId(""); joinLoader.open() } }
                        Item { Layout.fillWidth: true }
                        ActionButton { id: moonButton; objectName: "moonsButton"; text: "中转站"; onClicked: moonLoader.open() }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        ToolButton { objectName: "togglePeersButton"; text: backend.peersExpanded ? "▾" : "▸"; Accessible.name: backend.peersExpanded ? "收起成员" : "展开成员"; onClicked: backend.peersExpanded = !backend.peersExpanded }
                        Label { id: peersHeading; text: "成员"; font.pixelSize: 18; font.bold: true; color: Theme.text; TapHandler { onTapped: backend.peersExpanded = !backend.peersExpanded } HoverHandler { cursorShape: Qt.PointingHandCursor } }
                        Item { Layout.fillWidth: true }
                    }
                    Item {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        implicitHeight: backend.peersExpanded ? peers.implicitHeight : 0
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
                    Item { Layout.preferredHeight: 24 }
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
