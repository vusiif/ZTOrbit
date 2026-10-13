import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Popup {
    id: guide
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 140 } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 110 } }
    property int step: 0
    property var targets: []
    readonly property var texts: [
        "先检查 ZeroTier 服务状态。未安装时可安装，已安装后可启动服务。本指南不会修改配置。",
        "这里是本机节点 ID。加入网络后，将它提供给管理员，用于识别并授权你的设备。",
        "点击“加入网络”并填写管理员提供的 16 位网络 ID。",
        "网络状态正常后，双击网络行或点击展开按钮查看成员。右键可以复制 ID 或修改本机备注。",
        "成员列表显示直链、中转、IP 和延迟，便于检查连接状况。",
        "“中转站”用于管理 Moon。仅在管理员提供 Moon 配置时使用。"
    ]
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    signal targetRequested(var target)
    property rect targetRect: Qt.rect(0, 0, 0, 0)
    function reposition() {
        const target = targets[step]
        if (!target) { targetRect = Qt.rect(0, 0, 0, 0); return }
        const point = target.mapToItem(contentItem, 0, 0)
        targetRect = Qt.rect(Math.max(8, point.x - 5), Math.max(8, point.y - 5), Math.min(target.width + 10, width - point.x - 8), Math.min(target.height + 10, height - point.y - 8))
    }
    function reveal() { targetRequested(targets[step]); Qt.callLater(reposition) }
    onStepChanged: if (visible) { reveal(); stepReveal.restart() }
    NumberAnimation { id: stepReveal; target: card; property: "opacity"; from: 0.3; to: 1; duration: 140 }
    onWidthChanged: if (visible) Qt.callLater(reposition)
    onHeightChanged: if (visible) Qt.callLater(reposition)
    width: parent.width
    height: parent.height
    x: 0
    y: 0
    padding: 0
    dim: false
    onOpened: { step = 0; reveal() }
    background: Item {}
    contentItem: Item {
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }
        Rectangle { width: parent.width; height: Math.max(0, guide.targetRect.y); color: "#9941464c" }
        Rectangle { y: guide.targetRect.y; width: Math.max(0, guide.targetRect.x); height: guide.targetRect.height; color: "#9941464c" }
        Rectangle { x: guide.targetRect.x + guide.targetRect.width; y: guide.targetRect.y; width: Math.max(0, parent.width - x); height: guide.targetRect.height; color: "#9941464c" }
        Rectangle { y: guide.targetRect.y + guide.targetRect.height; width: parent.width; height: Math.max(0, parent.height - y); color: "#9941464c" }
        Rectangle { x: guide.targetRect.x; y: guide.targetRect.y; width: guide.targetRect.width; height: guide.targetRect.height; radius: 6; color: "transparent"; border.color: Theme.accent; border.width: 2 }
        Rectangle {
            id: card
            width: Math.min(430, parent.width - 32)
            height: content.implicitHeight + 48
            x: Math.max(16, Math.min(guide.targetRect.x, parent.width - width - 16))
            y: guide.targetRect.y + guide.targetRect.height + height + 20 < parent.height ? guide.targetRect.y + guide.targetRect.height + 16 : Math.max(16, guide.targetRect.y - height - 16)
            color: Theme.surface
            radius: 8
            border.color: Theme.accent
            ColumnLayout {
                id: content
                anchors.fill: parent
                anchors.margins: 24
                spacing: 18
                Label { text: "新手指南  " + (guide.step + 1) + "/" + guide.texts.length; font.pixelSize: 20; font.bold: true; color: Theme.text }
                Label { text: guide.texts[guide.step]; color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
                RowLayout {
                    ActionButton { text: "跳过"; focus: true; onClicked: guide.close() }
                    Item { Layout.fillWidth: true }
                    ActionButton { text: "上一步"; visible: guide.step > 0; onClicked: guide.step-- }
                    ActionButton { text: guide.step === guide.texts.length - 1 ? "完成" : "下一步"; primary: true; onClicked: { if (guide.step === guide.texts.length - 1) guide.close(); else guide.step++ } }
                }
            }
        }
    }
}
