import QtQuick
import QtQuick.Controls.Basic

ToolButton {
    id: control
    implicitWidth: 40
    implicitHeight: 40
    hoverEnabled: true
    Accessible.name: "设置"
    contentItem: GearIcon {}
    background: Rectangle {
        radius: 6
        color: control.down ? Theme.selection : control.hovered ? Theme.hover : "transparent"
        border.color: control.hovered || control.activeFocus ? Theme.accent : "transparent"
        Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 100 } }
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor }
    ToolTip.visible: hovered
    ToolTip.text: "设置"
}
