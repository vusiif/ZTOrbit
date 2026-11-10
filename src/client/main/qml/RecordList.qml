import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

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
        root.backend.setHostSetting(root.settingsKey, JSON.stringify(widths))
    }
    readonly property string settingsKey: "tables/qml/" + kind + "/widths"
    Component.onCompleted: {
        try { columnWidths = JSON.parse(root.backend.hostSetting(root.settingsKey, "[]")) } catch (e) { columnWidths = [] }
    }
    signal detailsRequested(string kind, string rowId, string name)
    signal exportRequested(string rowId)
    spacing: 0

    function edit(id, name) { editingText = name; editingId = id }
    function toggleSort(model, column) {
        if (!model) return
        model.sortBy(column, model.sortColumn === column ? !model.sortAscending : true)
    }
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
                    // Click a header to sort by it; click again to reverse.
                    Label {
                        objectName: "tableHeader-" + root.kind + "-" + header.index
                        anchors.fill: parent
                        anchors.rightMargin: 10
                        text: header.modelData + (root.records.sortColumn === header.index ? root.records.sortAscending ? "  ↑" : "  ↓" : "")
                        color: headerHover.hovered || root.records.sortColumn === header.index ? Theme.accent : Theme.muted
                        font.pixelSize: 12
                        font.bold: root.records.sortColumn === header.index
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                        HoverHandler { id: headerHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.toggleSort(root.records, header.index) }
                    }
                    MouseArea {
                        id: resizeHandle
                        anchors.right: parent.right
                        width: 10
                        height: parent.height
                        hoverEnabled: true
                        cursorShape: Qt.SplitHCursor
                        // A visible column edge so resizing can be discovered.
                        Rectangle {
                            visible: header.index < root.headers.length - 1
                            anchors.right: parent.right
                            anchors.rightMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            width: resizeHandle.containsMouse || resizeHandle.pressed ? 2 : 1
                            height: resizeHandle.containsMouse || resizeHandle.pressed ? parent.height - 8 : 14
                            color: resizeHandle.containsMouse || resizeHandle.pressed ? Theme.accent : Theme.border
                        }
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
                                id: expandButton
                                visible: root.kind === "networks"
                                implicitWidth: 24
                                implicitHeight: 24
                                focusPolicy: Qt.TabFocus
                                onClicked: root.backend.setExpanded(record.rowId, !record.expanded)
                                Accessible.name: record.expanded ? "收起网络成员" : "展开网络成员"
                                contentItem: Item {
                                    ChevronIcon {
                                        anchors.centerIn: parent
                                        width: 14; height: 14
                                        pointingRight: true
                                        rotation: record.expanded ? 90 : 0
                                        ink: expandButton.hovered ? Theme.accent : Theme.muted
                                        Behavior on rotation { NumberAnimation { duration: 160; easing.type: Easing.InOutCubic } }
                                    }
                                }
                                background: Rectangle { radius: 4; color: expandButton.hovered ? Theme.hover : "transparent"; border.color: expandButton.activeFocus ? Theme.accent : "transparent" }
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
                        Repeater {
                            // Member columns: 0 name, 1 IP (not sortable), 2 latency.
                            model: [{ title: "名称", column: 0, width: 180 }, { title: "内网 IP", column: -1, width: 240 }, { title: "延迟", column: 2, width: 110 }]
                            delegate: Label {
                                required property var modelData
                                readonly property var members: record.members
                                readonly property bool sorted: !!members && modelData.column >= 0 && members.sortColumn === modelData.column
                                objectName: "memberHeader-" + modelData.column
                                text: modelData.title + (sorted ? (members.sortAscending ? "  ↑" : "  ↓") : "")
                                color: modelData.column >= 0 && (memberHeaderHover.hovered || sorted) ? Theme.accent : Theme.muted
                                font.pixelSize: 12
                                font.bold: sorted
                                Layout.preferredWidth: modelData.width
                                HoverHandler { id: memberHeaderHover; enabled: modelData.column >= 0; cursorShape: Qt.PointingHandCursor }
                                TapHandler { enabled: modelData.column >= 0; onTapped: root.toggleSort(record.members, modelData.column) }
                            }
                        }
                    }
                Repeater {
                    model: record.members
                    delegate: Rectangle {
                        id: member
                        objectName: "networkMemberRow"
                        required property string rowId
                        required property string name
                        required property var details
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
                            Label { text: member.cells[0] || "—"; Layout.preferredWidth: 240; color: Theme.muted; elide: Text.ElideRight; Layout.maximumWidth: member.width * 0.5 }
                            Label {
                                id: memberLatency
                                text: member.cells.length > 1 ? member.cells[1] : "—"
                                color: Theme.muted
                                Layout.preferredWidth: 110
                                HoverHandler { id: latencyHover }
                                AppToolTip { parent: memberLatency; visible: latencyHover.hovered; text: member.details.latencySource || "" }
                            }
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
        text: root.statusText !== "" ? root.statusText : root.kind === "networks" ? "尚未加入网络。点击右上角的“加入网络”，填写管理员提供的网络 ID。" : root.kind === "moons" ? "还没有 Moon 文件，可以选择文件或拖入此处。" : "暂无可达成员。"
        color: Theme.muted
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.margins: 16
    }
        }
    }
}
