import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    property string message: ""
    property string heading: ""
    property bool error: false
    property bool working: false
    property bool shown: false
    property real maximumHeight: 250
    readonly property color messageColor: error ? Theme.noticeErrorText : Theme.noticeSuccessText
    implicitHeight: content.implicitHeight + 28
    opacity: shown ? 1 : 0
    visible: opacity > 0
    enabled: shown
    Accessible.role: Accessible.AlertMessage
    Accessible.name: heading + " " + message

    function present(text, isError = false, title = "", duration = 0, persistent = false) {
        if (!text || text.trim() === "") return
        message = text
        heading = title
        error = isError
        working = persistent
        shown = true
        holdTimer.stop()
        if (!persistent) {
            holdTimer.interval = duration > 0 ? duration : Math.min(10000, Math.max(isError ? 6500 : 4000, text.length * 45))
            holdTimer.restart()
        }
    }
    function dismiss() { shown = false; holdTimer.stop() }
    Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.InOutCubic } }
    Timer { id: holdTimer; onTriggered: root.dismiss() }
    HoverHandler {
        enabled: root.shown
        onHoveredChanged: {
            if (hovered) holdTimer.stop()
            else if (root.shown && !root.working) holdTimer.restart()
        }
    }
    Rectangle {
        objectName: "notificationBackground"
        anchors.fill: parent
        radius: 10
        color: root.error ? Theme.noticeError : Theme.noticeSuccess
        border.color: root.error ? Theme.noticeErrorBorder : Theme.noticeSuccessBorder
    }
    RowLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 14
        spacing: 12
        BusyIndicator { visible: root.working; running: visible && root.shown; Layout.preferredWidth: 22; Layout.preferredHeight: 22; Layout.alignment: Qt.AlignTop }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 5
            Label { text: root.heading; visible: text !== ""; color: root.messageColor; font.bold: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; Layout.fillWidth: true }
            BoundedScrollView {
                id: messageScroll
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(messageLabel.implicitHeight, Math.max(36, root.maximumHeight - 65))
                contentWidth: availableWidth
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                Label { id: messageLabel; objectName: "notificationMessage"; width: messageScroll.availableWidth; text: root.message; color: root.messageColor; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.pixelSize: 14 }
            }
        }
        ToolButton {
            objectName: "dismissNotificationButton"
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: 28
            Layout.preferredHeight: 28
            visible: root.shown
            Accessible.name: "关闭提示"
            onClicked: root.dismiss()
            contentItem: Text { text: "×"; font.pixelSize: 24; color: root.messageColor; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            background: Rectangle { color: parent.hovered || parent.activeFocus ? root.messageColor : "transparent"; opacity: 0.12; radius: 5 }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }
    }
}
