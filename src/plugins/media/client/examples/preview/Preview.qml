import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
Pane {
    required property var controller
    ColumnLayout {
        anchors.fill: parent
        Label { text: "媒体开发预览 · H.264 软件回送"; font.pixelSize: 22 }
        RowLayout {
            ComboBox { id: sources; Layout.fillWidth: true; model: controller.sources; textRole: "description"; valueRole: "source" }
            Button { text: "刷新来源"; onClicked: controller.refresh() }
            Button { text: "开始"; onClicked: controller.start(sources.currentValue || "synthetic") }
            Button { text: "停止"; onClicked: controller.stop() }
        }
        Label { text: controller.status; wrapMode: Text.Wrap; Layout.fillWidth: true }
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true
                Label { text: "采集预览" }
                Item { objectName: "sourcePreviewHost"; Layout.fillWidth: true; Layout.fillHeight: true }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true
                Label { text: "H.264 编码／解码回送" }
                Item { objectName: "decodedPreviewHost"; Layout.fillWidth: true; Layout.fillHeight: true }
            }
        }
    }
}
