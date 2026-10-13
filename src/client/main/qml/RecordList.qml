import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtCore

ColumnLayout {
    id: root
    required property var backend
    required property var records
    required property string kind
    required property var headers
    property string statusText: ""
    property string selectedId: ""
    property string editingId: ""
    property string editingText: ""
    property var columnWidths: []
    property int sortColumn: -1
    property bool sortAscending: true
    readonly property real columnsWidth: headers.reduce((sum, value, index) => sum + preferredWidth(index), 0)
    readonly property real tableWidth: Math.max(width, columnsWidth + 28)
    function preferredWidth(index) {
        const defaults = kind === "networks" ? [180, 180, 110, 240] : kind === "peers" ? [180, 100, 100, 200, 110] : [240]
        return Math.max(70, columnWidths[index] || defaults[index] || 140)
    }
    function columnWidth(index) {
        return preferredWidth(index) + (index === headers.length - 1 ? Math.max(0, tableWidth - columnsWidth - 28) : 0)
    }
    function resizeColumn(index, delta) {
        let widths = headers.map((value, column) => preferredWidth(column))
        widths[index] = Math.max(70, widths[index] + delta)
        columnWidths = widths
        columnSettings.widths = JSON.stringify(widths)
    }
    Settings { id: columnSettings; category: "tables/qml/" + root.kind; property string widths: "[]" }
    Component.onCompleted: { try { columnWidths = JSON.parse(columnSettings.widths) } catch (e) { columnWidths = [] } }
    signal detailsRequested(string kind, string rowId, string name)
    signal exportRequested(string rowId)
    spacing: 0

    function edit(id, name) { editingText = name; editingId = id }
    Flickable {
        id: horizontal
        Layout.fillWidth: true
        implicitHeight: table.implicitHeight + (contentWidth > width ? 14 : 0)
        contentWidth: root.tableWidth
        contentHeight: table.implicitHeight
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.horizontal: ScrollBar { policy: horizontal.contentWidth > horizontal.width ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }
        ColumnLayout {
            id: table
            width: root.tableWidth
            spacing: 0
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: 34
        color: Theme.side
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            spacing: 0
            Repeater {
                model: root.headers
                Item {
                    id: header
                    required property string modelData
                    required property int index
                    Layout.minimumWidth: root.columnWidth(index)
                    Layout.maximumWidth: root.columnWidth(index)
                    Layout.fillHeight: true
                    Label { anchors.fill: parent; text: header.modelData + (root.sortColumn === header.index ? root.sortAscending ? " ▴" : " ▾" : ""); color: Theme.muted; font.pixelSize: 12; verticalAlignment: Text.AlignVCenter }
                    TapHandler {
                        enabled: root.kind !== "peers"
                        onTapped: {
                            root.sortAscending = root.sortColumn === header.index ? !root.sortAscending : true
                            root.sortColumn = header.index
                            root.records.sortBy(root.sortColumn, root.sortAscending)
                        }
                    }
                    MouseArea {
                        anchors.right: parent.right
                        width: 10
                        height: parent.height
                        cursorShape: Qt.SplitHCursor
                        property real previousX: 0
                        onPressed: mouse => previousX = mapToItem(table, mouse.x, mouse.y).x
                        onPositionChanged: mouse => {
                            if (!pressed) return
                            const x = mapToItem(table, mouse.x, mouse.y).x
                            root.resizeColumn(header.index, x - previousX)
                            previousX = x
                        }
                    }
                }
            }
        }
    }
    Repeater {
        id: rows
        model: root.records
        delegate: ColumnLayout {
            id: record
            required property string rowId
            required property string name
            required property var cells
            required property var details
            required property bool expanded
            required property var members
            required property string memberStatus
            Layout.fillWidth: true
            spacing: 0
            Rectangle {
                id: line
                objectName: "recordRow-" + record.rowId
                Layout.fillWidth: true
                implicitHeight: 44
                color: root.selectedId === record.rowId ? Theme.selection : rowMouse.containsMouse ? Theme.hover : Theme.surface
                Behavior on color { enabled: !Theme.transitioning; ColorAnimation { duration: 100 } }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: mouse => {
                        root.selectedId = record.rowId
                        if (mouse.button === Qt.RightButton) contextMenuLoader.popup(mouse.x, mouse.y)
                    }
                    onDoubleClicked: { if (root.kind === "networks") root.backend.setExpanded(record.rowId, !record.expanded) }
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 0
                    Item {
                        Layout.minimumWidth: root.columnWidth(0)
                        Layout.maximumWidth: root.columnWidth(0)
                        Layout.fillHeight: true
                        RowLayout {
                            anchors.fill: parent
                            ToolButton {
                                visible: root.kind === "networks"
                                text: record.expanded ? "▾" : "▸"
                                implicitWidth: 22
                                onClicked: root.backend.setExpanded(record.rowId, !record.expanded)
                                Accessible.name: record.expanded ? "收起网络成员" : "展开网络成员"
                            }
                            Label { visible: root.editingId !== record.rowId; text: record.name; color: Theme.text; elide: Text.ElideRight; Layout.fillWidth: true }
                            Loader {
                                active: root.editingId === record.rowId
                                visible: active
                                Layout.fillWidth: true
                                sourceComponent: TextField {
                                    objectName: "remarkEditor"
                                    maximumLength: 128
                                    Component.onCompleted: { text = root.editingText; forceActiveFocus(); selectAll() }
                                    onTextEdited: root.editingText = text
                                    onEditingFinished: {
                                        if (root.editingId !== record.rowId) return
                                        root.backend.rename(root.kind, record.rowId, text)
                                        root.editingId = ""
                                    }
                                    Keys.onEscapePressed: { root.editingId = ""; root.forceActiveFocus() }
                                }
                            }
                        }
                    }
                    Repeater {
                        model: record.cells
                        Label { required property string modelData; required property int index; text: modelData; color: Theme.text; elide: Text.ElideRight; Layout.minimumWidth: root.columnWidth(index + 1); Layout.maximumWidth: root.columnWidth(index + 1) }
                    }
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border; opacity: 0.45 }
                LazyPopup {
                    id: contextMenuLoader
                    sourceComponent: AppMenu {
                    objectName: "recordContextMenu"
                    AppMenuItem { text: "复制 ID"; onTriggered: root.backend.copyText(record.rowId) }
                    AppMenuItem { text: "修改备注"; onTriggered: root.edit(record.rowId, record.name) }
                    AppMenuItem { text: "详细信息"; onTriggered: root.detailsRequested(root.kind, record.rowId, record.name) }
                    AppMenuItem {
                        visible: root.kind === "networks"
                        text: "复制本机 IP"
                        enabled: record.cells.length > 2 && record.cells[2] !== "—"
                        onTriggered: root.backend.copyText(record.cells[2])
                    }
                    AppMenuItem { visible: root.kind === "networks" && root.backend.hasMoon(record.rowId); text: "导出中转站"; onTriggered: root.exportRequested(record.rowId) }
                    MenuSeparator { visible: root.kind === "networks" || root.kind === "moons" }
                    AppMenuItem { visible: root.kind === "networks"; text: "退出网络"; enabled: !root.backend.busy; onTriggered: root.backend.leaveNetwork(record.rowId) }
                    AppMenuItem { visible: root.kind === "moons"; text: "删除并重启"; enabled: !root.backend.busy && !!record.details.moonFile; onTriggered: root.backend.deleteMoon(record.rowId) }
                    }
                }
            }
            Item {
                id: expandedArea
                readonly property bool opened: root.kind === "networks" && record.expanded
                Layout.fillWidth: true
                implicitHeight: opened ? memberContent.implicitHeight : 0
                visible: implicitHeight > 0
                clip: true
                opacity: opened ? 1 : 0
                Behavior on implicitHeight { NumberAnimation { duration: 160; easing.type: Easing.InOutCubic } }
                Behavior on opacity { NumberAnimation { duration: 130 } }
                ColumnLayout {
                    id: memberContent
                    width: parent.width
                    spacing: 0
                    RowLayout {
                        Layout.leftMargin: 46
                        Layout.preferredHeight: 28
                        Label { text: "名称"; color: Theme.muted; font.pixelSize: 12; Layout.preferredWidth: 180 }
                        Label { text: "内网 IP ▴"; color: Theme.muted; font.pixelSize: 12 }
                    }
                Repeater {
                    model: record.members
                    delegate: Rectangle {
                        id: member
                        objectName: "networkMemberRow"
                        required property string rowId
                        required property string name
                        required property var cells
                        property bool editing: false
                        Layout.fillWidth: true
                        implicitHeight: 44
                        color: Theme.side
                        MouseArea { anchors.fill: parent; acceptedButtons: Qt.RightButton; onClicked: mouse => memberMenuLoader.popup(mouse.x, mouse.y) }
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 46
                            anchors.rightMargin: 16
                            Label { visible: !member.editing; text: member.name; color: Theme.text; elide: Text.ElideRight; Layout.preferredWidth: 180; Layout.maximumWidth: 180 }
                            Loader {
                                active: member.editing
                                visible: active
                                Layout.preferredWidth: 180
                                Layout.maximumWidth: 180
                                sourceComponent: TextField {
                                    objectName: "networkMemberRemarkEditor"
                                    maximumLength: 128
                                    Component.onCompleted: { text = member.name; forceActiveFocus(); selectAll() }
                                    onEditingFinished: if (member.editing) { const value = text; member.editing = false; root.backend.rename("peers", member.rowId, value) }
                                    Keys.onEscapePressed: { member.editing = false; root.forceActiveFocus() }
                                }
                            }
                            Label { text: member.cells[0] || "—"; color: Theme.muted; elide: Text.ElideRight; Layout.maximumWidth: member.width * 0.5 }
                            Item { Layout.fillWidth: true }
                        }
                        LazyPopup {
                            id: memberMenuLoader
                            sourceComponent: AppMenu {
                                objectName: "networkMemberMenu"
                                AppMenuItem { text: member.rowId; onTriggered: root.backend.copyText(member.rowId) }
                                AppMenuItem { text: "修改备注"; onTriggered: member.editing = true }
                            }
                        }
                    }
                }
                Label { visible: record.memberStatus !== ""; text: record.memberStatus; wrapMode: Text.Wrap; color: Theme.muted; Layout.fillWidth: true; Layout.margins: 12 }
                }
            }
        }
    }
    Label {
        visible: root.records.count === 0 || root.statusText !== ""
        text: root.statusText !== "" ? root.statusText : root.kind === "networks" ? "尚未加入网络，点击下方按钮加入。" : root.kind === "moons" ? "还没有 Moon 文件，可以选择文件或拖入此处。" : "暂无可达成员。"
        color: Theme.muted
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.margins: 16
    }
        }
    }
}
