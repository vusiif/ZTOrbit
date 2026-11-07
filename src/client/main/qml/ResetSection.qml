import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Settings page section for resetting the local ZeroTier state. The host
// injects the reset controller so this file never reaches host internals.
ColumnLayout {
    id: root
    required property var reset
    spacing: 6

    ActionButton {
        id: resetButton
        objectName: "resetZeroTierStateButton"
        Layout.fillWidth: true
        text: root.reset && root.reset.busy ? "正在重置…" : "重置 ZeroTier 本地状态"
        destructive: true
        enabled: !!root.reset && root.reset.canReset
        Accessible.name: "重置 ZeroTier 本地状态"
        onClicked: if (root.reset) root.reset.requestReset()
    }
    Label {
        objectName: "resetZeroTierStatusLabel"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        visible: text !== ""
        text: root.reset ? (root.reset.lastError || root.reset.status || "") : ""
        color: root.reset && root.reset.lastError ? Theme.danger : Theme.muted
        font.pixelSize: 12
    }
}
