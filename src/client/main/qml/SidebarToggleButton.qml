import QtQuick
import QtQuick.Controls.Basic

// Always-reachable sidebar toggle. The chevron flips direction with the state
// and the label is exposed to assistive technology.
ToolButton {
    id: control
    objectName: "sidebarToggleButton"
    property bool collapsed: false
    signal toggleRequested()
    implicitWidth: 40
    implicitHeight: 40
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    activeFocusOnTab: true
    Accessible.name: control.collapsed ? "展开侧边栏" : "收起侧边栏"
    Accessible.description: "显示或隐藏菜单名称"
    contentItem: ChevronIcon {
        pointingRight: control.collapsed
        ink: control.hovered || control.activeFocus || control.down ? Theme.accent : Theme.text
    }
    background: Rectangle {
        radius: 6
        color: control.down || control.hovered ? Theme.hover : "transparent"
        border.color: control.activeFocus ? Theme.accent : "transparent"
        border.width: control.activeFocus ? 2 : 0
        Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 100 } }
    }
    HoverHandler { cursorShape: Qt.PointingHandCursor }
    AppToolTip {
        objectName: "sidebarToggleTooltip"
        parent: control
        visible: control.hovered
        text: control.collapsed ? "展开侧边栏" : "收起侧边栏"
    }
    onClicked: control.toggleRequested()
}
