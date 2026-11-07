import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property var update
    spacing: 8
    readonly property bool busy: !!root.update && root.update.busy
    onVisibleChanged: if (!visible) serverPopup.close()

    ActionButton {
        id: serverButton
        objectName: "updateServerButton"
        Layout.fillWidth: true
        text: "更新服务器"
        enabled: !root.busy
        onClicked: serverPopup.visible ? serverPopup.close() : serverPopup.open()
    }
    ActionButton {
        objectName: "checkUpdateButton"
        Layout.fillWidth: true
        text: root.busy ? "正在更新…" : "检查更新"
        enabled: !!root.update && root.update.configured && !root.busy
        onClicked: root.update.checkManually()
    }
    ProgressBar {
        objectName: "updateProgressBar"
        Layout.fillWidth: true
        visible: root.busy
        from: 0; to: 100
        value: root.update ? root.update.progress : 0
        indeterminate: root.update && root.update.state === "checking"
    }
    Label {
        objectName: "updateStatusLabel"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        visible: root.update && (root.busy || root.update.state === "failed" || root.update.state === "uptodate")
        text: root.update ? root.update.status : ""
        color: root.update && root.update.state === "failed" ? Theme.danger : Theme.text
        font.pixelSize: 12
    }
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
            Label { text: "更新服务器"; font.pixelSize: 16; font.bold: true; color: Theme.text }
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
                    background: Rectangle { radius: 5; color: Theme.field; border.color: ipInput.activeFocus ? Theme.accent : Theme.border }
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
                    background: Rectangle { radius: 5; color: Theme.field; border.color: portInput.activeFocus ? Theme.accent : Theme.border }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: serverPopup.formError !== ""
                text: serverPopup.formError
                wrapMode: Text.Wrap
                color: Theme.danger
                font.pixelSize: 12
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
