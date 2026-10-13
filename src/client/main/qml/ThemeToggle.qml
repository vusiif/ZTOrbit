import QtQuick
import QtQuick.Controls.Basic

AbstractButton {
    id: toggle
    checkable: true
    implicitWidth: 84
    implicitHeight: 36
    Accessible.name: checked ? "深色模式" : "浅色模式"
    Accessible.role: Accessible.CheckBox
    background: Rectangle {
        radius: height / 2
        color: toggle.checked ? "#30465f" : "#e2ecf5"
        border.color: toggle.activeFocus ? Theme.accent : Theme.border
        Behavior on color { ColorAnimation { duration: 180 } }
        Text { x: 12; anchors.verticalCenter: parent.verticalCenter; text: "☾"; color: "#d4e3f4"; font.pixelSize: 23; opacity: toggle.checked ? 1 : 0; Behavior on opacity { NumberAnimation { duration: 130 } } }
        Text { anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: "☀"; color: "#946219"; font.pixelSize: 21; opacity: toggle.checked ? 0 : 1; Behavior on opacity { NumberAnimation { duration: 130 } } }
        Rectangle {
            width: 28; height: 28; radius: 14; y: 4
            x: toggle.checked ? parent.width - width - 4 : 4
            color: toggle.checked ? "#bed3ed" : "#ffffff"
            Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.InOutCubic } }
            Behavior on color { ColorAnimation { duration: 180 } }
        }
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor }
    ToolTip.visible: hovered
    ToolTip.text: checked ? "深色模式" : "浅色模式"
}
