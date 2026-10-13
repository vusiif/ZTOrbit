import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ApplicationWindow {
    id: window
    required property string message
    width: 460
    height: 190
    visible: true
    title: "ZTGUI"
    color: Theme.window
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 18
        Label { text: window.message; color: Theme.text; textFormat: Text.PlainText; wrapMode: Text.Wrap; Layout.fillWidth: true; Layout.fillHeight: true }
        ActionButton { text: "关闭"; Layout.alignment: Qt.AlignRight; onClicked: window.close() }
    }
}
