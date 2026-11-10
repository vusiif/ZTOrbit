import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// A titled group of SettingRow items: the caption sits above the card and the
// rows inside share one surface separated by hairlines.
ColumnLayout {
    id: section
    property string title: ""
    default property alias rows: card.content
    spacing: 8
    Label {
        text: section.title
        visible: text !== ""
        color: Theme.muted
        font.pixelSize: Theme.fontCaption
        font.bold: true
        Layout.leftMargin: 4
    }
    Card {
        id: card
        Layout.fillWidth: true
        padding: 0
        spacing: 0
    }
}
