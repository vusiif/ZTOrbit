import QtQuick
import QtQuick.Controls.Basic

// The single themed on/off switch used by settings rows and popups.
Switch {
    id: control
    implicitWidth: 48
    implicitHeight: 28
    padding: 0
    indicator: Rectangle {
        anchors.centerIn: parent
        width: 44; height: 24; radius: 12
        color: control.checked ? Theme.primary : Theme.button
        border.color: control.activeFocus ? Theme.accent : control.checked ? Theme.primary : Theme.border
        border.width: control.activeFocus ? 2 : 1
        opacity: control.enabled ? 1 : 0.5
        Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 120 } }
        Rectangle {
            width: 18; height: 18; radius: 9; y: 3
            x: control.checked ? parent.width - width - 3 : 3
            color: control.checked ? Theme.primaryText : Theme.muted
            Behavior on x { NumberAnimation { duration: 140; easing.type: Easing.InOutCubic } }
        }
    }
    contentItem: Item {}
    HoverHandler { cursorShape: control.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
}
