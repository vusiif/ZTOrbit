import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control
    property bool primary: false
    property bool destructive: false
    implicitHeight: 36
    horizontalPadding: 16
    hoverEnabled: true
    contentItem: Text {
        text: control.text
        font: control.font
        color: !control.enabled ? Theme.muted : control.destructive ? Theme.danger : Theme.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        radius: 5
        color: control.down ? Theme.selection : control.hovered ? Theme.hover : control.flat ? "transparent" : control.primary ? Theme.selection : Theme.button
        border.color: control.activeFocus ? Theme.accent : control.flat ? "transparent" : Theme.border
        border.width: control.activeFocus ? 2 : 1
        opacity: control.enabled ? 1 : 0.55
        Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 100 } }
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor }
}
