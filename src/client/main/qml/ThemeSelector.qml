import QtQuick
import QtQuick.Controls.Basic

// Segmented theme choice: follow the system (default), light or dark.
Rectangle {
    id: root
    property string value: "system"
    signal selected(string value)
    readonly property var options: [
        { value: "system", label: "跟随系统" },
        { value: "light", label: "浅色" },
        { value: "dark", label: "深色" }
    ]
    implicitWidth: row.implicitWidth + 6
    implicitHeight: 34
    radius: Theme.radius + 2
    color: Theme.button
    border.color: Theme.border
    Accessible.role: Accessible.Grouping
    Accessible.name: "主题"
    Row {
        id: row
        anchors.centerIn: parent
        spacing: 2
        Repeater {
            model: root.options
            delegate: AbstractButton {
                id: option
                required property var modelData
                objectName: "themeOption-" + modelData.value
                readonly property bool current: root.value === modelData.value
                implicitWidth: Math.max(56, label.implicitWidth + 24)
                implicitHeight: 28
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                checkable: true
                checked: current
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.label
                onClicked: { if (!current) root.selected(modelData.value); checked = Qt.binding(() => current) }
                contentItem: Label {
                    id: label
                    text: option.modelData.label
                    font.pixelSize: Theme.fontCaption + 1
                    font.bold: option.current
                    color: option.current ? Theme.primaryText : Theme.text
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: Theme.radius
                    color: option.current ? Theme.primary : option.hovered ? Theme.hover : "transparent"
                    border.width: option.activeFocus ? 2 : 0
                    border.color: Theme.accent
                    Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 120 } }
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
            }
        }
    }
}
