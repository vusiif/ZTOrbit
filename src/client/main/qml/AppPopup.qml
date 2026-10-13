import QtQuick
import QtQuick.Controls.Basic

Popup {
    padding: 20
    focus: true
    background: Rectangle { color: Theme.popup; radius: 8; border.color: Theme.popupBorder; border.width: 1 }
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 140; easing.type: Easing.OutCubic } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 110; easing.type: Easing.InCubic } }
}
