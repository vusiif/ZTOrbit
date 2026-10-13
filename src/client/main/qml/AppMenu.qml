import QtQuick
import QtQuick.Controls.Basic

Menu {
    id: menu
    // Size to visible actions, never to the containing row or page.
    implicitWidth: {
        let result = 100
        for (let i = 0; i < count; ++i) {
            const action = itemAt(i)
            if (action && action.visible) result = Math.max(result, action.implicitWidth + leftPadding + rightPadding)
        }
        return result
    }
    padding: 6
    background: Rectangle { color: Theme.popup; radius: 7; border.color: Theme.popupBorder }
    delegate: AppMenuItem {}
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 130; easing.type: Easing.OutCubic } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 100 } }
}
