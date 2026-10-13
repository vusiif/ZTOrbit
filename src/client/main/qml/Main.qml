import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import QtCore
import Qt.labs.platform as Platform

ApplicationWindow {
    id: window
    objectName: "mainWindow"
    required property var backend
    property string exportNetworkId: ""
    property string detailKind: ""
    property string detailId: ""
    property var aboutPlugin: ({})
    property bool layoutReady: false
    property real guideScrollPosition: 0
    width: windowSettings.savedWidth
    height: windowSettings.savedHeight
    minimumWidth: 720
    minimumHeight: 480
    visible: true
    title: "ZTGUI"
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
    Component.onCompleted: { Theme.dark = Qt.binding(() => backend.dark); layoutReady = true }
    Settings { id: windowSettings; category: "window/qml"; property int savedWidth: 1024; property int savedHeight: 760; property real sidebarWidth: 190 }
    Component.onDestruction: {
        if (visibility !== Window.Maximized && visibility !== Window.Minimized) {
            windowSettings.savedWidth = Math.max(minimumWidth, width)
            windowSettings.savedHeight = Math.max(minimumHeight, height)
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
    function showDetails(kind, id, name) {
        detailKind = kind; detailId = id
        detailLoader.ensure().title = "详细信息 — " + name
        detailLoader.open()
    }
    function exportMoon(id) { exportNetworkId = id; exportLoader.ensure().currentFile = "file:///" + id + ".ztbundle"; exportLoader.open() }

    Platform.SystemTrayIcon {
        id: tray
        objectName: "systemTrayIcon"
        visible: true
        tooltip: backend.closing ? "ZTGUI · 正在退出" : "ZTGUI"
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
        function onNetworkJoined() { joinLoader.close() }
        function onConfigurationImported(networkId) { joinLoader.ensure().setNetworkId(networkId) }
        function onConfirmationRequested(title, message, acceptLabel) {
            const popup = confirmationLoader.ensure()
            popup.title = title; popup.message = message; popup.acceptLabel = acceptLabel; confirmationLoader.open()
        }
    }
    SplitView {
        anchors.fill: parent
        anchors.margins: 12
        orientation: Qt.Horizontal
        enabled: !backend.closing
        handle: Rectangle { implicitWidth: 10; color: "transparent" }
        Rectangle {
            id: sidebar
            SplitView.preferredWidth: windowSettings.sidebarWidth
            SplitView.minimumWidth: 150
            SplitView.maximumWidth: 270
            onWidthChanged: if (window.layoutReady && width >= 150) windowSettings.sidebarWidth = width
            color: Theme.side
            radius: 6
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 20
                RowLayout {
                    Rectangle {
                        objectName: "serviceStatusDot"
                        implicitWidth: 10; implicitHeight: 10; radius: 5
                        color: backend.service.available ? "#25a773" : "#d95059"
                        Accessible.name: backend.service.available ? "ZeroTier 在线" : "ZeroTier 离线"
                        ToolTip.visible: statusHover.hovered
                        ToolTip.text: backend.service.available ? "ZeroTier 在线" : "ZeroTier 离线"
                        HoverHandler { id: statusHover }
                        Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 140 } }
                    }
                    Label { text: "ZTGUI"; font.pixelSize: 24; font.bold: true; color: Theme.accent; Layout.fillWidth: true }
                    Label { text: "v" + backend.version; font.pixelSize: 11; color: Theme.muted }
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
                    delegate: ItemDelegate {
                        id: navigationItem
                        required property var modelData
                        width: navigation.width
                        height: 44
                        text: modelData.title
                        enabled: modelData.ready
                        highlighted: backend.activeMenu === modelData.id
                        onClicked: window.navigateTo(modelData.id)
                        contentItem: RowLayout {
                            spacing: 8
                            Image { visible: source.toString().length > 0; source: navigationItem.modelData.iconSource || ""; Layout.preferredWidth: 20; Layout.preferredHeight: 20 }
                            Label { text: navigationItem.text; color: navigationItem.enabled ? Theme.text : Theme.muted; font.pixelSize: 16; horizontalAlignment: Text.AlignLeft; verticalAlignment: Text.AlignVCenter; Layout.fillWidth: true }
                        }
                        background: Rectangle {
                            radius: 5
                            color: parent.highlighted ? Theme.selection : parent.hovered ? Theme.hover : "transparent"
                            Rectangle { visible: parent.parent.highlighted; width: 3; height: 22; anchors.verticalCenter: parent.verticalCenter; color: Theme.accent }
                        }
                        ToolTip.visible: hovered && (modelData.message !== "" || (modelData.toolTip || "") !== "")
                        ToolTip.text: modelData.message || modelData.toolTip || ""
                    }
                }
                GearButton { objectName: "settingsButton"; Layout.alignment: Qt.AlignLeft; onClicked: settingsLoader.open() }
            }
        }
        Item {
            id: pageHost
            SplitView.fillWidth: true
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
                                color: Theme.text
                                font.pixelSize: 28
                                font.bold: true
                                TapHandler { onTapped: backend.copyText(nodeId.text) }
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
                    Label { text: "成员"; font.pixelSize: 18; font.bold: true; color: Theme.text; Layout.leftMargin: 16 }
                    RecordList {
                        id: peers
                        objectName: "peers"
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        backend: window.backend
                        records: backend.peers
                        kind: "peers"
                        headers: ["名称", "角色", "连接", "IP 地址", "延迟（ms）"]
                        statusText: backend.peerStatus
                        onDetailsRequested: (kind, rowId, name) => window.showDetails(kind, rowId, name)
                    }
                    Item { Layout.preferredHeight: 24 }
                }
            }
            Item { id: pluginContainer; objectName: "pluginContainer"; anchors.fill: parent; visible: backend.activeMenu !== "builtin/zerotier" }
        }
    }
    footer: Pane {
        visible: backend.busy || backend.feedback !== "" || backend.closing
        padding: 12
        background: Rectangle { color: Theme.side }
        RowLayout {
            anchors.fill: parent
            BusyIndicator { running: backend.busy || backend.closing; visible: running; implicitWidth: 24; implicitHeight: 24 }
            Label { text: backend.closing ? backend.shutdownMessage : backend.feedback || "正在执行操作…"; wrapMode: Text.Wrap; color: Theme.text; Layout.fillWidth: true }
        }
    }
    LazyPopup {
        id: settingsLoader
        sourceComponent: AppPopup {
            id: settingsPopup
            objectName: "settingsPopup"
            width: Math.min(320, window.width - 40)
            x: 20
            y: Math.max(0, window.height - height - 70)
            padding: 20
            focus: true
            ColumnLayout {
                width: settingsPopup.availableWidth
                spacing: 14
                Label { text: "设置"; font.pixelSize: 18; font.bold: true }
                RowLayout {
                    Label { text: "主题" }
                    ThemeToggle {
                        objectName: "themeSelector"
                        checked: backend.dark
                        onClicked: backend.theme = checked ? "dark" : "light"
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: "禁止GPU加速"; Layout.fillWidth: true }
                    Switch {
                        id: gpuAccelerationSwitch
                        objectName: "disableGpuAccelerationSwitch"
                        Accessible.name: "禁止GPU加速"
                        implicitWidth: 52
                        implicitHeight: 32
                        checked: backend.gpuAccelerationForcedOff || backend.disableGpuAcceleration
                        enabled: !backend.gpuAccelerationForcedOff
                        onClicked: backend.disableGpuAcceleration = checked
                        indicator: Rectangle {
                            anchors.centerIn: parent
                            width: 48; height: 28; radius: 14
                            color: gpuAccelerationSwitch.checked ? Theme.accent : Theme.button
                            border.color: gpuAccelerationSwitch.activeFocus ? Theme.accent : Theme.border
                            opacity: gpuAccelerationSwitch.enabled ? 1 : 0.6
                            Rectangle {
                                width: 22; height: 22; radius: 11; y: 3
                                x: gpuAccelerationSwitch.checked ? parent.width - width - 3 : 3
                                color: gpuAccelerationSwitch.checked ? Theme.window : Theme.text
                                Behavior on x { NumberAnimation { duration: 140; easing.type: Easing.InOutCubic } }
                            }
                        }
                        HoverHandler { cursorShape: parent.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
                    }
                }
                Label {
                    objectName: "gpuAccelerationHint"
                    Layout.fillWidth: true
                    text: backend.gpuAccelerationForcedOff
                        ? "本次启动已由启动参数禁用GPU加速。"
                        : "开启后使用CPU绘制。\n完全退出并重启程序后生效。"
                    wrapMode: Text.Wrap
                    color: Theme.muted
                }
                ActionButton { text: "插件"; Layout.fillWidth: true; onClicked: { settingsLoader.close(); pluginLoader.open() } }
                ActionButton { text: "关于"; Layout.fillWidth: true; onClicked: { settingsLoader.close(); aboutLoader.open() } }
                ActionButton { text: "退出"; Layout.fillWidth: true; onClicked: backend.requestQuit() }
            }
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
                Label { text: backend.feedback; visible: text !== ""; wrapMode: Text.Wrap; color: Theme.danger; Layout.fillWidth: true }
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
                    ActionButton { text: "取消"; focus: true; onClicked: confirmation.reject() }
                    ActionButton { text: confirmation.acceptLabel; destructive: true; onClicked: confirmation.accept() }
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
            title: "插件"
            anchors.centerIn: parent
            width: Math.min(520, window.width - 40)
            height: Math.min(430, window.height - 60)
            modal: false
            closePolicy: Popup.CloseOnEscape
            ColumnLayout {
                anchors.fill: parent
                Label { text: "增减插件后重启 ZTGUI 生效。"; wrapMode: Text.Wrap; Layout.fillWidth: true; color: Theme.muted }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    model: backend.plugins
                    delegate: ItemDelegate {
                        required property var modelData
                        width: ListView.view.width
                        text: modelData.name + "  v" + modelData.version + (modelData.message ? "  " + modelData.message : "")
                        onClicked: { window.aboutPlugin = modelData; pluginAboutLoader.open() }
                        TapHandler { acceptedButtons: Qt.RightButton; onTapped: pluginMenu.popup() }
                        AppMenu {
                            id: pluginMenu
                            AppMenuItem { text: "加载"; enabled: false }
                            AppMenuItem { text: "卸载"; enabled: false }
                            AppMenuItem { text: "关于"; onTriggered: { window.aboutPlugin = modelData; pluginAboutLoader.open() } }
                        }
                    }
                    Label { anchors.centerIn: parent; visible: backend.plugins.length === 0; text: "没有已加载的插件"; color: Theme.muted }
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
                Label { text: "加载状态：" + (window.aboutPlugin.message || "已加载") }
            }
        }
    }
    LazyPopup {
        id: aboutLoader
        sourceComponent: AppDialog {
            id: aboutDialog
            title: "关于"
            anchors.centerIn: parent
            standardButtons: Dialog.Close
            ColumnLayout {
                anchors.fill: parent
                spacing: 12
                Label { text: "ZTGUI"; font.pixelSize: 24; font.bold: true }
                Label { text: "v" + backend.version }
                Label { text: "API " + backend.apiVersion }
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
            targets: [serviceButton, nodeId, joinButton, networks, peers, moonButton]
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
        sourceComponent: FileDialog { id: importBundleFile; title: "导入中转站"; nameFilters: ["中转站配置 (*.ztbundle *.moon)"]; onAccepted: backend.importConfiguration(selectedFile) }

    }
    LazyPopup {
        id: importMoonLoader
        sourceComponent: FileDialog { id: importMoonFile; title: "选择 Moon 文件"; fileMode: FileDialog.OpenFiles; nameFilters: ["Moon (*.moon)"]; onAccepted: backend.importMoons(selectedFiles) }

    }
    LazyPopup {
        id: exportLoader
        sourceComponent: FileDialog { id: exportFile; title: "导出中转站"; fileMode: FileDialog.SaveFile; nameFilters: ["中转站配置包 (*.ztbundle)"]; onAccepted: backend.exportConfiguration(window.exportNetworkId, selectedFile) }

    }
    LazyPopup {
        id: pluginFileLoader
        sourceComponent: FileDialog { id: pluginFile; title: "导入插件"; nameFilters: ["插件 (*.dll *.dylib *.so)"]; onAccepted: backend.importPlugin(selectedFile) }

    }
}
