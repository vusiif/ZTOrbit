import QtQuick
import QtQuick.Controls.Basic

// Three visual weights: primary (filled, one per area), default (outlined) and
// flat (text only). Destructive tints any weight with the danger colour.
Button {
    id: control
    property bool primary: false
    property bool destructive: false
    implicitHeight: 34
    horizontalPadding: 14
    hoverEnabled: true
    font.pixelSize: Theme.fontBody
    readonly property color fill: {
        if (control.primary) return control.down || control.hovered ? Theme.primaryHover : Theme.primary
        if (control.destructive) return control.down ? Theme.dangerSoft : control.hovered ? Theme.dangerSoft : control.flat ? "transparent" : Theme.button
        return control.down ? Theme.selection : control.hovered ? Theme.hover : control.flat ? "transparent" : Theme.button
    }
    contentItem: Text {
        text: control.text
        font: control.font
        color: !control.enabled ? Theme.muted : control.primary ? Theme.primaryText : control.destructive ? Theme.danger : Theme.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: Theme.radius
        color: control.fill
        border.color: control.activeFocus ? Theme.accent : control.flat || control.primary ? "transparent" : control.destructive ? Theme.danger : Theme.border
        border.width: control.activeFocus ? 2 : 1
        opacity: control.enabled ? 1 : 0.5
        Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 100 } }
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor }
}
