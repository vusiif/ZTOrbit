import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Pane {
    id: root
    objectName: "settingsPage"
    required property var backend
    signal openPlugins()
    signal openAbout()
    function placePopup(popup, anchor) {
        const area = Overlay.overlay
        if (!area || !popup || !anchor || !popup.parent) return
        const point = anchor.mapToItem(area, 0, anchor.height)
        const x = Math.max(12, Math.min(point.x, area.width - popup.width - 12))
        const y = Math.max(12, Math.min(point.y + 4, area.height - popup.height - 12))
        const local = area.mapToItem(popup.parent, x, y)
        popup.x = local.x
        popup.y = local.y
    }
    Connections {
        target: root.Window.window
        function onWidthChanged() { root.repositionPopups() }
        function onHeightChanged() { root.repositionPopups() }
    }
    function repositionPopups() {
        if (tcpFallbackPopup && tcpFallbackPopup.visible) placePopup(tcpFallbackPopup, tcpButton)
        if (relaySelector && relaySelector.popup && relaySelector.popup.visible) placePopup(relaySelector.popup, relaySelector)
    }
    padding: 20
    background: Rectangle { color: Theme.surface; radius: 8 }
    onVisibleChanged: {
        if (visible) backend.refreshLocalConfig()
        else tcpFallbackPopup.close()
    }
    component SectionTitle: Label {
        Layout.fillWidth: true
        Layout.topMargin: 8
        font.pixelSize: 16
        font.bold: true
        color: Theme.text
    }
    component FullButton: ActionButton { Layout.fillWidth: true; implicitHeight: 36 }
    BoundedScrollView {
        id: settingsScroll
        objectName: "settingsScroll"
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: settingsScroll.availableWidth
            spacing: 10
            Label { text: "设置"; font.pixelSize: 24; font.bold: true; color: Theme.text }
            SectionTitle { text: "外观" }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "主题"; Layout.fillWidth: true }
                ThemeToggle { objectName: "themeSelector"; checked: backend.dark; onClicked: backend.theme = checked ? "dark" : "light" }
            }
            SectionTitle { text: "程序" }
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
            FullButton { objectName: "openPluginsButton"; text: "插件"; onClicked: root.openPlugins() }
            UpdateSection {
                objectName: "updateSection"
                Layout.fillWidth: true
                update: root.backend.update
            }
            SectionTitle { text: "ZeroTier" }
            FullButton {
                id: tcpButton
                objectName: "switchTcpFallbackServerButton"
                text: "TCP回退"
                onClicked: tcpFallbackPopup.visible ? tcpFallbackPopup.close() : tcpFallbackPopup.open()
            }
            FullButton {
                objectName: "openLocalConfigButton"; text: "ZeroTier配置文件"
                onClicked: backend.revealLocalConfig()
            }
            ResetSection {
                objectName: "resetSection"
                Layout.fillWidth: true
                reset: root.backend.reset
            }
            SectionTitle { text: "存储" }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "日志"; Layout.fillWidth: true }
                ActionButton { objectName: "openLogsButton"; text: "打开日志"; onClicked: backend.openLogs()}
                ActionButton { objectName: "cleanupLogsButton"; text: "清理日志"; enabled: !backend.storageBusy && !backend.storageScanBusy; onClicked: backend.cleanupStorage("logs")}
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "缓存"; Layout.fillWidth: true }
                ActionButton { objectName: "cleanupCacheButton"; text: "清理缓存"; enabled: !backend.storageBusy && !backend.storageScanBusy; onClicked: backend.cleanupStorage("cache")}
            }
            FullButton { objectName: "openSettingsButton"; text: "打开程序设置文件"; onClicked: backend.revealSettings()}
            // Low-frequency diagnostics stay available without crowding the page.
            ToolButton { objectName: "storageDetailsButton"; text: "存储详情"; Layout.alignment: Qt.AlignRight; onClicked: { backend.refreshStorageUsage(); storagePopup.open() } }
            SectionTitle { text: "其他" }
            FullButton { objectName: "aboutButton"; text: "关于"; onClicked: root.openAbout() }
            FullButton { text: "退出"; onClicked: backend.requestQuit() }
        }
    }
    AppPopup {
        id: tcpFallbackPopup
        objectName: "tcpFallbackRelayPopup"
        parent: Overlay.overlay
        width: Math.min(380, parent.width - 24)
        height: Math.min(implicitHeight, parent.height - 24)
        onHeightChanged: if (visible) root.placePopup(tcpFallbackPopup, tcpButton)
        onWidthChanged: if (visible) root.placePopup(tcpFallbackPopup, tcpButton)
        padding: 14
        modal: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onAboutToShow: {
            root.placePopup(tcpFallbackPopup, tcpButton)
            backend.refreshLocalConfig(); backend.refreshTcpFallbackServers()
            Qt.callLater(root.repositionPopups)
        }
        onAboutToHide: relaySelector.popup.close()
        contentItem: ColumnLayout {
            spacing: 10
            RowLayout {
                Layout.fillWidth: true
                Label { text: "强制回退"; Layout.fillWidth: true }
                    Switch {
                        id: forceTcpFallbackSwitch
                        objectName: "forceTcpFallbackSwitch"
                        Accessible.name: "强制回退"
                        implicitWidth: 52
                        implicitHeight: 32
                        checked: backend.localConfig.forceTcpRelay === true
                        enabled: backend.localConfig.loaded
                            && backend.localConfig.valid && !backend.localConfig.busy
                        onClicked: { backend.setTcpFallbackSwitches(true, checked); checked = Qt.binding(() => backend.localConfig.forceTcpRelay === true) }
                        indicator: Rectangle {
                            anchors.centerIn: parent
                            width: 48; height: 28; radius: 14
                            color: forceTcpFallbackSwitch.checked ? Theme.accent : Theme.button
                            border.color: forceTcpFallbackSwitch.activeFocus ? Theme.accent : Theme.border
                            opacity: forceTcpFallbackSwitch.enabled ? 1 : 0.6
                            Rectangle {
                                width: 22; height: 22; radius: 11; y: 3
                                x: forceTcpFallbackSwitch.checked ? parent.width - width - 3 : 3
                                color: forceTcpFallbackSwitch.checked ? Theme.window : Theme.text
                                Behavior on x { NumberAnimation { duration: 140; easing.type: Easing.InOutCubic } }
                            }
                        }
                        HoverHandler { cursorShape: parent.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
                    }
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "中转服务器"; Layout.fillWidth: true }
                ComboBox {
                    id: relaySelector
                    implicitHeight: 32
                    objectName: "tcpFallbackServerSelector"
                    Layout.preferredWidth: Math.min(220, tcpFallbackPopup.width - 130)
                    Layout.minimumWidth: 100
                    model: backend.tcpFallbackServers
                    textRole: "name"
                    valueRole: "networkId"
                    currentIndex: -1
                    onActivated: index => {
                        const candidate = backend.tcpFallbackServers[index]
                        currentIndex = -1
                        if (!candidate || !candidate.enabled) return
                        tcpFallbackPopup.close()
                        backend.selectTcpFallbackServer(candidate.networkId)
                    }
                    displayText: backend.tcpFallbackNetworkName
                    enabled: !backend.tcpFallbackServersBusy && backend.localConfig.loaded && backend.localConfig.valid && !backend.localConfig.busy
                    contentItem: Label { text: relaySelector.displayText; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter; color: relaySelector.enabled ? Theme.text : Theme.muted; leftPadding: 10; rightPadding: 22 }
                    indicator: Label { text: "▾"; color: Theme.muted; anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter }
                    background: Rectangle { color: Theme.field; radius: 5; border.color: relaySelector.activeFocus ? Theme.accent : Theme.border }
                    popup: AppPopup {
                        parent: relaySelector
                        onAboutToShow: { root.placePopup(relaySelector.popup, relaySelector); Qt.callLater(root.repositionPopups) }
                        onHeightChanged: if (visible) root.placePopup(relaySelector.popup, relaySelector)
                        onWidthChanged: if (visible) root.placePopup(relaySelector.popup, relaySelector)
                        width: Math.min(420, tcpFallbackPopup.parent.width - 24)
                        height: Math.min(300, implicitHeight, tcpFallbackPopup.parent.height - 24)
                        padding: 6
                        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                        contentItem: ListView {
                            id: relayList
                            objectName: "tcpFallbackServerList"
                            implicitHeight: Math.min(280, contentHeight)
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            model: relaySelector.popup.visible ? backend.tcpFallbackServers : null
                            delegate: ItemDelegate {
                                id: candidate
                                required property var modelData
                                objectName: "tcpFallbackServer-" + modelData.networkId
                                width: ListView.view.width
                                enabled: modelData.enabled
                                onClicked: { relaySelector.popup.close(); tcpFallbackPopup.close(); backend.selectTcpFallbackServer(modelData.networkId) }
                                contentItem: ColumnLayout {
                                    spacing: 3
                                    Label { text: candidate.modelData.name; elide: Text.ElideRight; Layout.fillWidth: true; color: candidate.enabled ? Theme.text : Theme.muted; font.bold: true }
                                    Label { text: candidate.modelData.networkId; color: Theme.muted; font.pixelSize: 12 }
                                    Label { text: candidate.modelData.reason || ""; visible: text !== ""; wrapMode: Text.Wrap; Layout.fillWidth: true; color: Theme.danger; font.pixelSize: 12 }
                                }
                                background: Rectangle { radius: 4; color: candidate.highlighted ? Theme.selection : candidate.hovered ? Theme.hover : "transparent" }
                            }
                            ScrollBar.vertical: ScrollBar {}
                        }
                    }
                }
            }
            Label {
                objectName: "tcpFallbackServersMessage"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: text !== ""
                text: backend.tcpFallbackServersMessage || ""
                font.pixelSize: 12
                color: backend.tcpFallbackServersBusy ? Theme.muted : Theme.danger
            }
            Label { objectName: "localConfigErrorLabel"; text: backend.localConfig.error || ""; visible: text !== ""; wrapMode: Text.Wrap; Layout.fillWidth: true; color: Theme.danger; font.pixelSize: 12 }
            ActionButton { text: "重试读取"; visible: !!backend.localConfig.error || (!backend.tcpFallbackServersBusy && backend.tcpFallbackServers.length === 0); onClicked: { backend.retryLocalConfig(); backend.refreshTcpFallbackServers() } }

        }
    }
    AppPopup {
        id: storagePopup
        objectName: "storageDetailsPopup"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(520, parent.width - 32)
        height: Math.min(implicitHeight, parent.height - 32)
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        contentItem: BoundedScrollView {
            id: storageScroll
            contentWidth: availableWidth
            implicitHeight: Math.min(420, storageContent.implicitHeight)
            ColumnLayout {
                id: storageContent
                width: storageScroll.availableWidth
                Label { text: "存储详情"; font.bold: true; font.pixelSize: 16 }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted; text: "模式：" + (backend.storageUsage.mode === "personal" ? "个人目录" : backend.storageUsage.mode === "ephemeral" ? "临时目录" : "程序共享目录") + "\n日志 " + backend.storageUsage.logFiles + " 个文件，" + (backend.storageUsage.logsBytes / 1048576).toFixed(1) + " MiB；插件数据 " + (backend.storageUsage.pluginDataBytes / 1048576).toFixed(1) + " MiB；缓存 " + (backend.storageUsage.cacheBytes / 1048576).toFixed(1) + " MiB" }
                Label { objectName: "storagePathLabel"; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; text: "活动目录：" + backend.storageUsage.root + "\n设置文件：" + backend.storageUsage.settings + "\n日志目录：" + backend.storageUsage.logs + "\n插件数据：" + backend.storageUsage.pluginData + "\n缓存：" + backend.storageUsage.cache; color: Theme.muted }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: (backend.storageUsage.previewText || "") + "\n" + (backend.storageUsage.maintenance || "") + "\n" + (backend.storageUsage.pendingDeletions || "") + "\n" + (backend.storageUsage.orphansText || ""); color: Theme.muted }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: backend.storageScanBusy ? "正在后台扫描存储…" : (backend.storageUsage.scanError || "") + "\n" + (backend.storageWarning || ""); color: Theme.danger }
                ActionButton { objectName: "cleanupOrphansButton"; text: "检查并清理无主数据"; enabled: !backend.storageBusy && !backend.storageScanBusy; onClicked: { storagePopup.close(); backend.cleanupStorage("orphans") } }
            }
        }
    }
}
