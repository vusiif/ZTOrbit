import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Settings row for application updates: status on the left, the server
// configuration and the check action on the right, progress underneath.
ColumnLayout {
    id: root
    required property var update
    spacing: 0
    readonly property bool busy: !!root.update && root.update.busy
    readonly property bool showStatus: !!root.update && (root.busy || root.update.state === "failed" || root.update.state === "uptodate")
    onVisibleChanged: if (!visible) serverPopup.close()

    SettingRow {
        title: "软件更新"
        description: root.showStatus ? root.update.status
            : root.update && root.update.configured ? "从更新服务器获取新版本"
            : "尚未配置更新服务器"
        descriptionColor: root.update && root.update.state === "failed" ? Theme.danger : Theme.muted
        ActionButton {
            id: serverButton
            objectName: "updateServerButton"
            text: "服务器…"
            flat: true
            enabled: !root.busy
            onClicked: serverPopup.visible ? serverPopup.close() : serverPopup.open()
        }
        ActionButton {
            objectName: "checkUpdateButton"
            text: root.busy ? "正在更新…" : "检查更新"
            enabled: !!root.update && root.update.configured && !root.busy
            onClicked: root.update.checkManually()
        }
    }
    ProgressBar {
        objectName: "updateProgressBar"
        Layout.fillWidth: true
        Layout.leftMargin: 16
        Layout.rightMargin: 16
        Layout.bottomMargin: 12
        visible: root.busy
        from: 0; to: 100
        value: root.update ? root.update.progress : 0
        indeterminate: root.update && root.update.state === "checking"
    }
    // Kept addressable for diagnostics; the row description shows the text.
    Label { objectName: "updateStatusLabel"; visible: false; text: root.update ? root.update.status : "" }
    AppPopup {
        id: serverPopup
        objectName: "updateServerPopup"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(360, parent.width - 32)
        modal: false
        padding: 16
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        property string formError: ""
        function save() {
            if (!root.update) return
            const port = ipInput.text.trim() === "" ? 18102 : Number(portInput.text)
            if (root.update.setServer(ipInput.text, port)) close()
            else formError = root.update.lastError || "无法保存更新服务器"
        }
        onAboutToShow: {
            ipInput.text = root.update ? root.update.serverIp : ""
            portInput.text = String(root.update ? root.update.serverPort : 18102)
            formError = ""
            Qt.callLater(() => ipInput.forceActiveFocus())
        }
        contentItem: ColumnLayout {
            spacing: 12
            Label { text: "更新服务器"; font.pixelSize: Theme.fontHeading; font.bold: true; color: Theme.text }
            Label { text: "留空 IP 并保存可清除设置。"; color: Theme.muted; font.pixelSize: Theme.fontCaption; wrapMode: Text.Wrap; Layout.fillWidth: true }
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 12
                rowSpacing: 10
                Label { text: "IP"; color: Theme.text }
                TextField {
                    id: ipInput
                    objectName: "updateServerIpInput"
                    Accessible.name: "更新服务器 IP"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    selectByMouse: true
                    color: Theme.text
                    selectionColor: Theme.selection
                    selectedTextColor: Theme.text
                    onAccepted: serverPopup.save()
                    background: Rectangle { radius: Theme.radius; color: Theme.field; border.color: ipInput.activeFocus ? Theme.accent : Theme.border }
                }
                Label { text: "端口"; color: Theme.text }
                TextField {
                    id: portInput
                    objectName: "updateServerPortInput"
                    Accessible.name: "更新服务器端口"
                    Layout.fillWidth: true
                    selectByMouse: true
                    text: "18102"
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: IntValidator { bottom: 1; top: 65535 }
                    color: Theme.text
                    selectionColor: Theme.selection
                    selectedTextColor: Theme.text
                    onAccepted: serverPopup.save()
                    background: Rectangle { radius: Theme.radius; color: Theme.field; border.color: portInput.activeFocus ? Theme.accent : Theme.border }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: serverPopup.formError !== ""
                text: serverPopup.formError
                wrapMode: Text.Wrap
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
            }
            ActionButton {
                objectName: "saveUpdateServerButton"
                Layout.fillWidth: true
                text: "保存"
                primary: true
                enabled: !root.busy && (ipInput.text.trim() === "" || portInput.acceptableInput)
                onClicked: serverPopup.save()
            }
        }
    }
}
