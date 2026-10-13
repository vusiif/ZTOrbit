import QtQuick
import QtQuick.Controls.Basic

MenuItem {
    id: action
    implicitWidth: label.implicitWidth + 32
    implicitHeight: visible ? 36 : 0
    height: implicitHeight
    padding: 8
    leftPadding: 12
    rightPadding: 12
    contentItem: Text {
        id: label
        text: action.text
        font: action.font
        color: action.enabled ? Theme.text : Theme.muted
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        radius: 4
        color: action.highlighted ? Theme.selection : "transparent"
        Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 100 } }
    }
}
