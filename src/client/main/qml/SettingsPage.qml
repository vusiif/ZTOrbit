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
        const x = Math.max(12, Math.min(point.x + anchor.width - popup.width, area.width - popup.width - 12))
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
    padding: 0
    background: Item {}
    onVisibleChanged: {
        if (visible) backend.refreshLocalConfig()
        else tcpFallbackPopup.close()
    }
    BoundedScrollView {
        id: settingsScroll
        objectName: "settingsScroll"
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        Item {
            width: settingsScroll.availableWidth
            implicitHeight: settingsColumn.implicitHeight + 24
            // A readable column: rows stretch with the window up to 720 px,
            // then stay centred instead of pushing controls to the far edge.
            ColumnLayout {
                id: settingsColumn
                width: Math.min(parent.width - 32, 720)
                x: (parent.width - width) / 2
                y: 8
                spacing: 20
                Label { text: "设置"; font.pixelSize: Theme.fontTitle; font.bold: true; color: Theme.text }

                SectionCard {
                    title: "外观"
                    Layout.fillWidth: true
                    SettingRow {
                        title: "主题"
                        description: backend.theme === "system" ? "跟随系统（当前" + (backend.dark ? "深色" : "浅色") + "）"
                            : backend.dark ? "深色" : "浅色"
                        showDivider: false
                        ThemeSelector { objectName: "themeSelector"; value: backend.theme; onSelected: value => backend.theme = value }
                    }
                }

                SectionCard {
                    title: "程序"
                    Layout.fillWidth: true
                    SettingRow {
                        title: "插件"
                        description: "加载、卸载或导入插件"
                        showDivider: false
                        ActionButton { objectName: "openPluginsButton"; text: "管理"; onClicked: root.openPlugins() }
                    }
                    UpdateSection {
                        objectName: "updateSection"
                        Layout.fillWidth: true
                        update: root.backend.update
                    }
                    SettingRow {
                        title: "禁用 GPU 加速"
                        description: backend.gpuAccelerationForcedOff ? "当前环境已强制使用 CPU 绘制" : "使用 CPU 绘制界面，重新启动后生效"
                        AppSwitch {
                            id: gpuAccelerationSwitch
                            objectName: "disableGpuAccelerationSwitch"
                            Accessible.name: "禁用 GPU 加速"
                            checked: backend.gpuAccelerationForcedOff || backend.disableGpuAcceleration
                            enabled: !backend.gpuAccelerationForcedOff
                            onClicked: backend.disableGpuAcceleration = checked
                        }
                    }
                }

                SectionCard {
                    title: "ZeroTier"
                    Layout.fillWidth: true
                    SettingRow {
                        title: "TCP 回退"
                        description: (backend.localConfig.forceTcpRelay === true ? "已强制回退" : "UDP 不通时自动回退")
                            + (backend.tcpFallbackNetworkName ? " · " + backend.tcpFallbackNetworkName : "")
                        showDivider: false
                        ActionButton {
                            id: tcpButton
                            objectName: "switchTcpFallbackServerButton"
                            text: "配置…"
                            onClicked: tcpFallbackPopup.visible ? tcpFallbackPopup.close() : tcpFallbackPopup.open()
                        }
                    }
                    SettingRow {
                        title: "ZeroTier 配置文件"
                        description: "在文件管理器中显示 local.conf"
                        ActionButton { objectName: "openLocalConfigButton"; text: "显示"; onClicked: backend.revealLocalConfig() }
                    }
                    ResetSection {
                        objectName: "resetSection"
                        reset: root.backend.reset
                    }
                }

                SectionCard {
                    title: "存储"
                    Layout.fillWidth: true
                    SettingRow {
                        title: "日志"
                        showDivider: false
                        ActionButton { objectName: "openLogsButton"; text: "打开"; flat: true; onClicked: backend.openLogs() }
                        ActionButton { objectName: "cleanupLogsButton"; text: "清理"; enabled: !backend.storageBusy && !backend.storageScanBusy; onClicked: backend.cleanupStorage("logs") }
                    }
                    SettingRow {
                        title: "缓存"
                        ActionButton { objectName: "cleanupCacheButton"; text: "清理"; enabled: !backend.storageBusy && !backend.storageScanBusy; onClicked: backend.cleanupStorage("cache") }
                    }
                    SettingRow {
                        title: "程序设置文件"
                        description: "在文件管理器中显示"
                        ActionButton { objectName: "openSettingsButton"; text: "显示"; onClicked: backend.revealSettings() }
                    }
                    SettingRow {
                        title: "存储详情"
                        description: "占用空间、目录位置与无主数据"
                        ActionButton { objectName: "storageDetailsButton"; text: "查看"; onClicked: { backend.refreshStorageUsage(); storagePopup.open() } }
                    }
                }

                SectionCard {
                    title: "其他"
                    Layout.fillWidth: true
                    SettingRow {
                        title: "关于"
                        description: "v" + backend.version + " · API " + backend.apiVersion
                        showDivider: false
                        ActionButton { objectName: "aboutButton"; text: "详情"; onClicked: root.openAbout() }
                    }
                    SettingRow {
                        title: "退出程序"
                        description: "关闭窗口并退出托盘"
                        ActionButton { objectName: "quitButton"; text: "退出"; destructive: true; onClicked: backend.requestQuit() }
                    }
                }
            }
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
        padding: 16
        modal: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onAboutToShow: {
            root.placePopup(tcpFallbackPopup, tcpButton)
            backend.refreshLocalConfig(); backend.refreshTcpFallbackServers()
            Qt.callLater(root.repositionPopups)
        }
        onAboutToHide: relaySelector.popup.close()
        contentItem: ColumnLayout {
            spacing: 12
            Label { text: "TCP 回退"; font.pixelSize: Theme.fontHeading; font.bold: true; color: Theme.text }
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label { text: "强制回退"; color: Theme.text }
                    Label { text: "始终经 TCP 中转，即使 UDP 可用"; color: Theme.muted; font.pixelSize: Theme.fontCaption; wrapMode: Text.Wrap; Layout.fillWidth: true }
                }
                AppSwitch {
                    id: forceTcpFallbackSwitch
                    objectName: "forceTcpFallbackSwitch"
                    Accessible.name: "强制回退"
                    checked: backend.localConfig.forceTcpRelay === true
                    enabled: backend.localConfig.loaded
                        && backend.localConfig.valid && !backend.localConfig.busy
                    onClicked: { backend.setTcpFallbackSwitches(true, checked); checked = Qt.binding(() => backend.localConfig.forceTcpRelay === true) }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Label { text: "中转服务器"; color: Theme.text; Layout.fillWidth: true }
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
                    contentItem: Label { text: relaySelector.displayText; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter; color: relaySelector.enabled ? Theme.text : Theme.muted; leftPadding: 10; rightPadding: 26 }
                    indicator: ChevronIcon { width: 14; height: 14; pointingRight: true; rotation: 90; ink: Theme.muted; anchors.right: parent.right; anchors.rightMargin: 9; anchors.verticalCenter: parent.verticalCenter }
                    background: Rectangle { color: Theme.field; radius: Theme.radius; border.color: relaySelector.activeFocus ? Theme.accent : Theme.border }
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
                                    Label { text: candidate.modelData.networkId; color: Theme.muted; font.pixelSize: Theme.fontCaption }
                                    Label { text: candidate.modelData.reason || ""; visible: text !== ""; wrapMode: Text.Wrap; Layout.fillWidth: true; color: Theme.danger; font.pixelSize: Theme.fontCaption }
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
                font.pixelSize: Theme.fontCaption
                color: backend.tcpFallbackServersBusy ? Theme.muted : Theme.danger
            }
            Label { objectName: "localConfigErrorLabel"; text: backend.localConfig.error || ""; visible: text !== ""; wrapMode: Text.Wrap; Layout.fillWidth: true; color: Theme.danger; font.pixelSize: Theme.fontCaption }
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
                spacing: 10
                Label { text: "存储详情"; font.bold: true; font.pixelSize: Theme.fontHeading; color: Theme.text }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted; text: "模式：" + (backend.storageUsage.mode === "personal" ? "个人目录" : backend.storageUsage.mode === "ephemeral" ? "临时目录" : "程序共享目录") + "\n日志 " + backend.storageUsage.logFiles + " 个文件，" + (backend.storageUsage.logsBytes / 1048576).toFixed(1) + " MiB；插件数据 " + (backend.storageUsage.pluginDataBytes / 1048576).toFixed(1) + " MiB；缓存 " + (backend.storageUsage.cacheBytes / 1048576).toFixed(1) + " MiB" }
                Label { objectName: "storagePathLabel"; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; text: "活动目录：" + backend.storageUsage.root + "\n设置文件：" + backend.storageUsage.settings + "\n日志目录：" + backend.storageUsage.logs + "\n插件数据：" + backend.storageUsage.pluginData + "\n缓存：" + backend.storageUsage.cache; color: Theme.muted }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: [backend.storageUsage.previewText, backend.storageUsage.maintenance, backend.storageUsage.pendingDeletions, backend.storageUsage.orphansText].filter(s => !!s).join("\n"); visible: text !== ""; color: Theme.muted }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: backend.storageScanBusy ? "正在后台扫描存储…" : [backend.storageUsage.scanError, backend.storageWarning].filter(s => !!s).join("\n"); visible: text !== ""; color: backend.storageScanBusy ? Theme.muted : Theme.danger }
                ActionButton { objectName: "cleanupOrphansButton"; text: "检查并清理无主数据"; enabled: !backend.storageBusy && !backend.storageScanBusy; onClicked: { storagePopup.close(); backend.cleanupStorage("orphans") } }
            }
        }
    }
}
