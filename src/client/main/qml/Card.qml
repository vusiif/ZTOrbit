import QtQuick
import QtQuick.Layouts

// Raised content block on the window background. Children stack vertically
// inside `padding`; tables pass padding 0 and add their own margins.
Rectangle {
    id: card
    property int padding: 16
    default property alias content: body.data
    property alias spacing: body.spacing
    implicitWidth: body.implicitWidth + padding * 2
    implicitHeight: body.implicitHeight + padding * 2
    radius: Theme.cardRadius
    color: Theme.surface
    border.color: Theme.cardBorder
    ColumnLayout {
        id: body
        x: card.padding
        y: card.padding
        width: card.width - card.padding * 2
        spacing: 12
    }
}
