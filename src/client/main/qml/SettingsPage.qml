import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
Pane {
    id: root
    objectName: "settingsPage"
    required property var backend
    signal openPlugins()
    signal openAbout()
    padding: 20
    background: Rectangle { color: Theme.surface; radius: 8 }
    onVisibleChanged: if (visible) backend.refreshLocalConfig()
    BoundedScrollView {
        id: settingsScroll
        objectName: "settingsScroll"
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
            ColumnLayout {
                width: settingsScroll.availableWidth
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
                Label { text: "本机 ZeroTier TCP 回退"; font.pixelSize: 16; font.bold: true; Layout.topMargin: 8 }
                Label {
                    objectName: "localConfigScopeLabel"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    color: Theme.muted
                    text: "这些是本机 ZeroTier 节点级设置，影响本机加入的所有网络。保存后不会自动重启 ZeroTier。"
                }
                Label {
                    objectName: "localConfigPathLabel"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    color: Theme.muted
                    text: "配置文件：" + (backend.localConfig.path || "")
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: "允许TCP回退"; Layout.fillWidth: true }
                    Switch {
                        id: allowTcpFallbackSwitch
                        objectName: "allowTcpFallbackSwitch"
                        Accessible.name: "允许TCP回退"
                        implicitWidth: 52
                        implicitHeight: 32
                        checked: backend.localConfig.allowTcpFallbackRelay === true
                        enabled: backend.localConfig.loaded && backend.localConfig.valid && !backend.localConfig.busy
                        onClicked: backend.setTcpFallbackSwitches(checked, forceTcpFallbackSwitch.checked)
                        indicator: Rectangle {
                            anchors.centerIn: parent
                            width: 48; height: 28; radius: 14
                            color: allowTcpFallbackSwitch.checked ? Theme.accent : Theme.button
                            border.color: allowTcpFallbackSwitch.activeFocus ? Theme.accent : Theme.border
                            opacity: allowTcpFallbackSwitch.enabled ? 1 : 0.6
                            Rectangle {
                                width: 22; height: 22; radius: 11; y: 3
                                x: allowTcpFallbackSwitch.checked ? parent.width - width - 3 : 3
                                color: allowTcpFallbackSwitch.checked ? Theme.window : Theme.text
                                Behavior on x { NumberAnimation { duration: 140; easing.type: Easing.InOutCubic } }
                            }
                        }
                        HoverHandler { cursorShape: parent.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: "强制TCP回退"; Layout.fillWidth: true }
                    Switch {
                        id: forceTcpFallbackSwitch
                        objectName: "forceTcpFallbackSwitch"
                        Accessible.name: "强制TCP回退"
                        implicitWidth: 52
                        implicitHeight: 32
                        checked: backend.localConfig.forceTcpRelay === true
                        enabled: allowTcpFallbackSwitch.checked && backend.localConfig.loaded
                            && backend.localConfig.valid && !backend.localConfig.busy
                        onClicked: backend.setTcpFallbackSwitches(allowTcpFallbackSwitch.checked, checked)
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
                    Label {
                        objectName: "localConfigRelayLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: "当前中转服务器：" + (backend.localConfig.relayText || "未设置")
                    }
                    ActionButton {
                        objectName: "refreshLocalConfigButton"
                        text: "重新读取"
                        flat: true
                        implicitHeight: 28
                        font.pixelSize: 11
                        onClicked: backend.retryLocalConfig()
                    }
                }
                Label {
                    objectName: "localConfigErrorLabel"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: text !== ""
                    color: Theme.danger
                    font.pixelSize: 11
                    text: backend.localConfig.error || ""
                }
                RowLayout {
                    Layout.fillWidth: true
                    ActionButton {
                        id: switchTcpFallbackServerButton
                        objectName: "switchTcpFallbackServerButton"
                        text: "切换TCP回退中转服务器"
                        enabled: backend.localConfig.loaded && backend.localConfig.valid && !backend.localConfig.busy
                        onClicked: tcpFallbackPopup.opened ? tcpFallbackPopup.close() : tcpFallbackPopup.open()
                    }
                    ActionButton {
                        objectName: "retryLocalConfigButton"
                        text: "重试读取"
                        flat: true
                        implicitHeight: 28
                        font.pixelSize: 11
                        visible: backend.localConfig.canRetry
                        onClicked: backend.retryLocalConfig()
                    }
                }
                Popup {
                    id: tcpFallbackPopup
                    objectName: "tcpFallbackRelayPopup"
                    parent: switchTcpFallbackServerButton
                    x: 0
                    y: switchTcpFallbackServerButton.height + 4
                    width: Math.min(560, settingsScroll.availableWidth - 20)
                    padding: 12
                    modal: false
                    focus: true
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                    background: Rectangle { color: Theme.popup; radius: 8; border.color: Theme.popupBorder; border.width: 1 }
                    onOpened: backend.refreshTcpFallbackServers()
                    contentItem: ColumnLayout {
                        spacing: 8
                        Label { text: "选择网络（与 .z2bundle 导出条件一致）"; font.bold: true; color: Theme.text }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            font.pixelSize: 11
                            color: Theme.muted
                            text: "服务器公网 IPv4 来自对应 Moon 服务器根节点的 stableEndpoints；中继端口固定 9443。"
                        }
                        Label {
                            visible: backend.tcpFallbackServersBusy
                            text: "正在读取网络与 Moon 信息…"
                            color: Theme.muted
                            font.pixelSize: 11
                        }
                        Label {
                            objectName: "tcpFallbackServersMessage"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            visible: text !== ""
                            color: Theme.danger
                            font.pixelSize: 11
                            text: backend.tcpFallbackServersMessage || ""
                        }
                        ListView {
                            id: tcpFallbackServerList
                            objectName: "tcpFallbackServerList"
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.min(260, contentHeight)
                            visible: count > 0
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            model: backend.tcpFallbackServers
                            delegate: ItemDelegate {
                                id: tcpFallbackServerItem
                                required property var modelData
                                objectName: "tcpFallbackServer-" + modelData.networkId
                                width: ListView.view.width
                                enabled: modelData.enabled
                                onClicked: {
                                    tcpFallbackPopup.close()
                                    backend.selectTcpFallbackServer(modelData.networkId)
                                }
                                contentItem: ColumnLayout {
                                    spacing: 2
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            text: tcpFallbackServerItem.modelData.name
                                                + (tcpFallbackServerItem.modelData.current ? "（当前）" : "")
                                            font.bold: true
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                            color: Theme.text
                                        }
                                        Label {
                                            text: tcpFallbackServerItem.modelData.enabled
                                                ? tcpFallbackServerItem.modelData.relay : "不可用"
                                            color: tcpFallbackServerItem.modelData.enabled ? Theme.accent : Theme.muted
                                            font.pixelSize: 11
                                        }
                                    }
                                    Label {
                                        text: tcpFallbackServerItem.modelData.networkId
                                        font.pixelSize: 10
                                        color: Theme.muted
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.Wrap
                                        visible: text !== ""
                                        text: tcpFallbackServerItem.modelData.reason
                                        color: Theme.danger
                                        font.pixelSize: 10
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.Wrap
                                        visible: text !== ""
                                        text: tcpFallbackServerItem.modelData.note
                                        color: Theme.muted
                                        font.pixelSize: 10
                                    }
                                }
                                background: Rectangle {
                                    radius: 4
                                    color: tcpFallbackServerItem.highlighted ? Theme.selection
                                        : tcpFallbackServerItem.hovered ? Theme.hover : "transparent"
                                    opacity: tcpFallbackServerItem.enabled ? 1 : 0.65
                                }
                            }
                        }
                        Label {
                            objectName: "tcpFallbackServersEmptyLabel"
                            visible: !backend.tcpFallbackServersBusy && backend.tcpFallbackServers.length === 0
                            text: "暂无可选网络"
                            color: Theme.muted
                            font.pixelSize: 11
                        }
                        ActionButton { text: "关闭"; Layout.alignment: Qt.AlignRight; onClicked: tcpFallbackPopup.close() }
                    }
                }
                ActionButton { objectName: "openPluginsButton"; text: "插件"; Layout.fillWidth: true; onClicked: { root.openPlugins() } }
                Label { text: "存储与清理"; font.pixelSize: 16; font.bold: true; Layout.topMargin: 8 }
                ColumnLayout {
                    objectName: "storageSection"
                    Layout.fillWidth: true
                    spacing: 6
                    Label {
                        objectName: "storagePathLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        font.pixelSize: 11
                        color: Theme.muted
                        text: "模式：" + (backend.storageUsage.mode === "personal" ? "个人目录（程序目录不可写）"
                            : backend.storageUsage.mode === "ephemeral" ? "临时目录（无法持久保存）" : "程序共享目录")
                            + "\n活动目录：" + backend.storageUsage.root
                            + "\n设置文件：" + backend.storageUsage.settings
                            + "\n日志目录：" + backend.storageUsage.logs
                            + "\n插件数据：" + backend.storageUsage.pluginData
                            + "\n可丢弃缓存：" + backend.storageUsage.cache
                    }
                    Label {
                        objectName: "storageUsageLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        font.pixelSize: 11
                        text: "日志 " + backend.storageUsage.logFiles + " 个文件，"
                            + (backend.storageUsage.logsBytes / 1048576).toFixed(1) + " MiB；插件数据 "
                            + (backend.storageUsage.pluginDataBytes / 1048576).toFixed(1) + " MiB；缓存 "
                            + (backend.storageUsage.cacheBytes / 1048576).toFixed(1) + " MiB"
                    }
                    Label {
                        objectName: "storagePreviewLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        font.pixelSize: 11
                        text: backend.storageUsage.previewText || ""
                    }
                    Label {
                        objectName: "storageOrphansLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        font.pixelSize: 11
                        color: Theme.muted
                        text: backend.storageUsage.orphansText || ""
                    }
                    Label {
                        objectName: "storageScanStateLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        font.pixelSize: 11
                        color: Theme.muted
                        visible: text !== ""
                        text: backend.storageScanBusy ? "正在后台扫描存储…"
                            : backend.storageBusy ? "正在清理存储…"
                            : (backend.storageUsage.scanError || "")
                    }
                    Label {
                        objectName: "storageMaintenanceLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        visible: text !== ""
                        color: Theme.muted
                        font.pixelSize: 11
                        text: (backend.storageUsage.maintenance || "") + (backend.storageUsage.pendingDeletions
                            ? (backend.storageUsage.maintenance ? "\n" : "") + backend.storageUsage.pendingDeletions : "")
                    }
                    Label {
                        objectName: "storageWarningLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        visible: text !== ""
                        color: Theme.danger
                        text: backend.storageWarning
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        ActionButton { objectName: "cleanupLogsButton"; text: "清理历史日志"; enabled: !backend.storageBusy && !backend.storageScanBusy; onClicked: backend.cleanupStorage("logs") }
                        ActionButton { objectName: "cleanupCacheButton"; text: "清理闲置缓存"; enabled: !backend.storageBusy && !backend.storageScanBusy; onClicked: backend.cleanupStorage("cache") }
                    }
                    ActionButton {
                        objectName: "cleanupOrphansButton"
                        text: "检查并清理无主数据"
                        Layout.fillWidth: true
                        enabled: !backend.storageBusy && !backend.storageScanBusy
                        onClicked: { backend.cleanupStorage("orphans") }
                    }
                    Label { Layout.fillWidth: true; wrapMode: Text.Wrap; visible: text !== ""; text: backend.feedback; color: Theme.muted; font.pixelSize: 11 }
                }
                ActionButton { objectName: "aboutButton"; text: "关于"; Layout.fillWidth: true; onClicked: { root.openAbout() } }
                ActionButton { text: "退出"; Layout.fillWidth: true; onClicked: backend.requestQuit() }
                ActionButton {
                    objectName: "openLocalConfigButton"
                    text: "打开本地配置文件"
                    flat: true
                    implicitHeight: 28
                    font.pixelSize: 11
                    horizontalPadding: 10
                    Layout.alignment: Qt.AlignLeft
                    onClicked: backend.revealLocalConfig()
                }
            }
    }
}
