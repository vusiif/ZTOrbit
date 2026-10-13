import QtQuick
import QtQuick.Controls.Basic

ScrollView {
    id: scroll
    function constrain() {
        if (contentItem && contentItem instanceof Flickable) {
            contentItem.boundsBehavior = Flickable.StopAtBounds
            contentItem.boundsMovement = Flickable.StopAtBounds
        }
    }
    onContentItemChanged: Qt.callLater(constrain)
    Component.onCompleted: constrain()
}
