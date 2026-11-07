import QtQuick
import QtQuick.Controls.Basic

ToolButton {
    id: control
    property bool selected: false
    implicitWidth: 40
    implicitHeight: 40
    hoverEnabled: true
    Accessible.name: "设置"
    contentItem: GearIcon { ink: control.hovered || control.activeFocus ? Theme.accent : Theme.text }
    background: Rectangle {
        radius: 6
        color: control.down || control.selected ? Theme.selection : control.hovered ? Theme.hover : "transparent"
        border.color: control.hovered || control.activeFocus ? Theme.accent : "transparent"
        Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 100 } }
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor }
    AppToolTip { objectName: "settingsTooltip"; parent: control; visible: control.hovered; text: "设置" }
}
