import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// One setting: what it is and what it does on the left, its control(s) on the
// right. Rows after the first draw a hairline above themselves.
Item {
    id: row
    property string title: ""
    property string description: ""
    property color descriptionColor: Theme.muted
    property bool showDivider: true
    default property alias controls: controlRow.data
    Layout.fillWidth: true
    implicitHeight: Math.max(56, content.implicitHeight + 24)
    Rectangle {
        visible: row.showDivider
        x: 16
        width: parent.width - 32
        height: 1
        color: Theme.cardBorder
    }
    RowLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        spacing: 16
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                text: row.title
                color: Theme.text
                font.pixelSize: Theme.fontBody
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                text: row.description
                visible: text !== ""
                color: row.descriptionColor
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }
        RowLayout {
            id: controlRow
            spacing: 8
            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
        }
    }
}
