import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Settings row for resetting the local ZeroTier state. The host injects the
// reset controller so this file never reaches host internals.
SettingRow {
    id: root
    required property var reset
    readonly property string feedback: root.reset ? (root.reset.lastError || root.reset.status || "") : ""
    title: "重置本地状态"
    description: feedback !== "" ? feedback : "退出全部网络，清除成员缓存、Moon、自定义 planet 与过期中转设置"
    descriptionColor: root.reset && root.reset.lastError ? Theme.danger : Theme.muted

    ActionButton {
        id: resetButton
        objectName: "resetZeroTierStateButton"
        text: root.reset && root.reset.busy ? "正在重置…" : "重置"
        destructive: true
        enabled: !!root.reset && root.reset.canReset
        Accessible.name: "重置 ZeroTier 本地状态"
        onClicked: if (root.reset) root.reset.requestReset()
    }
    // Kept addressable for diagnostics; the row description shows the text.
    Label { objectName: "resetZeroTierStatusLabel"; visible: false; text: root.feedback }
}
