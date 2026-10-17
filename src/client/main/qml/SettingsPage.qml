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
                ActionButton { objectName: "aboutButton"; text: "关于"; Layout.fillWidth: true; onClicked: { root.openAbout() } }
                ActionButton { text: "退出"; Layout.fillWidth: true; onClicked: backend.requestQuit() }
            }
    }
}
