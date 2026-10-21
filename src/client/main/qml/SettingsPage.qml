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
    BoundedScrollView {
        id: settingsScroll
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
            }
    }
}
