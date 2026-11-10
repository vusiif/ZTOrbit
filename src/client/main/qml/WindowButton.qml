import QtQuick
import QtQuick.Controls.Basic

// Caption button for the custom title bar: minimize, maximize/restore or close.
AbstractButton {
    id: control
    property string kind: "minimize"   // minimize | maximize | restore | close
    readonly property bool closeKind: kind === "close"
    readonly property color glyph: closeKind && (hovered || down) ? "#ffffff" : Theme.text
    implicitWidth: 46
    implicitHeight: 34
    hoverEnabled: true
    focusPolicy: Qt.NoFocus
    Accessible.role: Accessible.Button
    background: Rectangle {
        color: control.closeKind
            ? (control.down ? "#f1707a" : control.hovered ? "#e81123" : "transparent")
            : (control.down ? Theme.selection : control.hovered ? Theme.hover : "transparent")
        Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 90 } }
    }
    contentItem: Item {
        // 10 px glyphs drawn from rectangles so they stay crisp at any scale.
        Rectangle {
            visible: control.kind === "minimize"
            anchors.centerIn: parent
            width: 10; height: 1; color: control.glyph
        }
        Rectangle {
            visible: control.kind === "maximize"
            anchors.centerIn: parent
            width: 10; height: 10; color: "transparent"
            border.width: 1; border.color: control.glyph
        }
        Item {
            visible: control.kind === "restore"
            anchors.centerIn: parent
            width: 10; height: 10
            Rectangle { x: 2; y: 0; width: 8; height: 8; color: "transparent"; border.width: 1; border.color: control.glyph }
            Rectangle {
                x: 0; y: 2; width: 8; height: 8; border.width: 1; border.color: control.glyph
                color: control.background.color.a > 0 ? control.background.color : Theme.window
            }
        }
        Item {
            visible: control.closeKind
            anchors.centerIn: parent
            width: 14; height: 14
            Rectangle { anchors.centerIn: parent; width: 14; height: 1; rotation: 45; antialiasing: true; color: control.glyph }
            Rectangle { anchors.centerIn: parent; width: 14; height: 1; rotation: -45; antialiasing: true; color: control.glyph }
        }
    }
}
