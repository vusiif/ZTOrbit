import QtQuick
import QtQuick.Controls.Basic

Dialog {
    id: dialog
    property color panelColor: Theme.popup
    padding: 20
    background: Rectangle { color: dialog.panelColor; radius: Theme.cardRadius; border.color: Theme.popupBorder; border.width: 1 }
    header: Label {
        text: dialog.title
        visible: text !== ""
        leftPadding: 20
        rightPadding: 20
        topPadding: 16
        bottomPadding: 14
        font.pixelSize: 17
        font.bold: true
        color: Theme.text
        // Rounded only at the top so the header meets the body in a straight
        // hairline instead of a notch.
        background: Item {
            Rectangle { anchors.fill: parent; anchors.margins: 1; radius: Theme.cardRadius - 1; color: Theme.popupHeader }
            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.leftMargin: 1; anchors.rightMargin: 1; height: Theme.cardRadius; color: Theme.popupHeader }
            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.cardBorder }
        }
    }
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 140; easing.type: Easing.OutCubic } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 110; easing.type: Easing.InCubic } }
}
