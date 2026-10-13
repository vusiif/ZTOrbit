import QtQuick
import QtQuick.Controls.Basic

Dialog {
    id: dialog
    property color panelColor: Theme.popup
    padding: 20
    background: Rectangle { color: dialog.panelColor; radius: 8; border.color: Theme.popupBorder; border.width: 1 }
    header: Label {
        text: dialog.title
        visible: text !== ""
        padding: 20
        font.pixelSize: 19
        font.bold: true
        color: Theme.text
        background: Rectangle { color: Theme.popupHeader; radius: 8 }
    }
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 140; easing.type: Easing.OutCubic } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 110; easing.type: Easing.InCubic } }
}
