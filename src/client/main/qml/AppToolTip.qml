import QtQuick
import QtQuick.Controls.Basic
ToolTip {
    id: control
    delay: 2000
    text: ""
    contentItem: Text { text: control.text; color: Theme.text; wrapMode: Text.Wrap }
    background: Rectangle { color: Theme.popup; border.color: Theme.popupBorder; radius: 5 }
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 160 } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 120 } }
}
