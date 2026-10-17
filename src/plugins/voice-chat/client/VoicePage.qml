import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Pane {
    id: page
    objectName: "voiceChatPage"
    required property var controller
    property int selectedRoom: 0
    readonly property bool dark: palette.window.hslLightness < 0.5
    property color panelColor: dark ? "#303643" : "#edf1f7"
    property color memberColor: dark ? "#343e46" : "#eaf2f2"
    property color popupColor: dark ? "#3d374a" : "#f3eef8"
    property color edgeColor: dark ? "#8593ae" : "#9caec3"
    property color buttonColor: dark ? "#485971" : "#dae6f5"
    property color selectedColor: dark ? "#425a7b" : "#c9def5"
    property color ink: palette.windowText
    readonly property bool themeTransitioning: panelAnimation.running
    Behavior on panelColor { ColorAnimation { id: panelAnimation; duration: 220 } }
    Behavior on memberColor { ColorAnimation { duration: 220 } }
    Behavior on popupColor { ColorAnimation { duration: 220 } }
    Behavior on buttonColor { ColorAnimation { duration: 220 } }
    Behavior on selectedColor { ColorAnimation { duration: 220 } }
    background: Rectangle { color: page.palette.window }
    padding: 0
    onVisibleChanged: if (!visible) {
        if (audioLoader.item) audioLoader.item.close()
        if (memberLoader.item) memberLoader.item.close()
    }
    component VoiceButton: Button {
        id: button
        implicitHeight: 36
        horizontalPadding: 14
        hoverEnabled: true
        contentItem: Text { text: button.text; font: button.font; color: page.ink; opacity: button.enabled ? 1 : 0.5; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: 5; color: button.down || button.hovered ? page.selectedColor : page.buttonColor; border.color: page.edgeColor; opacity: button.enabled ? 1 : 0.5; Behavior on color { enabled: !page.themeTransitioning; ColorAnimation { duration: 120 } } }
        HoverHandler { cursorShape: Qt.PointingHandCursor }
    }
    component VoiceField: TextField {
        id: field
        implicitHeight: 36
        color: page.ink
        placeholderTextColor: page.dark ? "#a9bdcb" : "#607687"
        selectByMouse: true
        background: Rectangle { radius: 4; color: page.palette.base; border.color: field.activeFocus ? page.palette.highlight : page.edgeColor; border.width: field.activeFocus ? 2 : 1 }
    }
    component Panel: GroupBox {
        id: panel
        property color fillColor: page.panelColor
        padding: 14
        topPadding: 36
        label: Label { x: 14; y: 10; text: panel.title; color: page.ink; font.bold: true }
        background: Rectangle { radius: 7; color: panel.fillColor; border.color: page.edgeColor }
    }
    component BoundedScroll: ScrollView {
        function constrain() { if (contentItem instanceof Flickable) { contentItem.boundsBehavior = Flickable.StopAtBounds; contentItem.boundsMovement = Flickable.StopAtBounds } }
        onContentItemChanged: Qt.callLater(constrain)
        Component.onCompleted: constrain()
    }
    component AudioChoice: ComboBox {
        id: choice
        property string selectedValue: ""
        function syncSelection() {
            for (let i = 0; i < count; ++i) if (valueAt(i) === selectedValue) { currentIndex = i; return }
            currentIndex = count > 0 ? 0 : -1
        }
        onModelChanged: Qt.callLater(syncSelection)
        onSelectedValueChanged: Qt.callLater(syncSelection)
        Component.onCompleted: Qt.callLater(syncSelection)
        textRole: "label"
        valueRole: "id"
        implicitHeight: 36
        contentItem: Text { text: choice.displayText; color: page.ink; font: choice.font; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight; leftPadding: 10; rightPadding: 25 }
        background: Rectangle { radius: 4; color: page.palette.base; border.color: page.edgeColor }
        delegate: ItemDelegate {
            id: option
            required property var modelData
            required property int index
            width: choice.width
            text: modelData.label
            highlighted: choice.highlightedIndex === index
            contentItem: Text { text: option.text; font: choice.font; color: page.ink; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
            background: Rectangle { color: option.highlighted ? page.selectedColor : page.popupColor }
        }
        popup: Popup {
            y: choice.height + 4
            width: choice.width
            padding: 5
            implicitHeight: Math.min(contentItem.implicitHeight + 10, 240)
            background: Rectangle { color: page.popupColor; border.color: page.edgeColor; radius: 5 }
            contentItem: ListView { clip: true; implicitHeight: contentHeight; model: choice.popup.visible ? choice.delegateModel : null; currentIndex: choice.highlightedIndex; boundsBehavior: Flickable.StopAtBounds; ScrollBar.vertical: ScrollBar {} }
            enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 160 } }
            exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 120 } }
        }
    }
    Connections {
        target: controller
        function onRoomsChanged() {
            let found = false
            for (let room of controller.rooms) if (room.id === page.selectedRoom) found = true
            if (!found) page.selectedRoom = 0
        }
    }
    BoundedScroll {
        id: scroll
        objectName: "voiceScroll"
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: scroll.availableWidth
            spacing: 18
            Label { text: "语音聊天"; font.pixelSize: 26; font.bold: true; Layout.topMargin: 20; Layout.leftMargin: 20 }
            Label { text: "填写语音服务器地址，连接后选择房间。"; Layout.leftMargin: 20; Layout.rightMargin: 20; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Panel {
                title: "服务器"
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                RowLayout {
                    anchors.fill: parent
                    spacing: 16
                    ColumnLayout {
                        Layout.fillWidth: true
                        RowLayout {
                            Label { text: "服务器"; Layout.preferredWidth: 48 }
                            VoiceField { objectName: "voiceServerAddress"; placeholderText: "IP / 域名，可选 :端口"; text: controller.address; onTextEdited: controller.address = text; enabled: !controller.connected && !controller.busy; Layout.fillWidth: true }
                        }
                        RowLayout {
                            Label { text: "昵称"; Layout.preferredWidth: 48 }
                            VoiceField { objectName: "voiceNickname"; placeholderText: "留空使用 ZeroTier ID"; text: controller.nickname; maximumLength: 64; onTextEdited: controller.nickname = text; enabled: !controller.connected && !controller.busy; Layout.fillWidth: true }
                        }
                    }
                    VoiceButton { objectName: "voiceConnect"; text: controller.connected ? "断开" : controller.busy ? "取消" : "连接"; onClicked: controller.toggleConnection() }
                    VoiceButton {
                        id: audioGear
                        objectName: "voiceAudioSettings"
                        implicitWidth: 40
                        contentItem: Canvas {
                            property color ink: page.ink
                            onInkChanged: requestPaint()
                            onPaint: {
                                const ctx = getContext("2d")
                                ctx.reset(); ctx.translate(width / 2, height / 2)
                                ctx.strokeStyle = ink; ctx.lineWidth = 1.7; ctx.lineJoin = "round"
                                ctx.beginPath()
                                for (let i = 0; i < 8; ++i) {
                                    const a = i * Math.PI / 4
                                    const points = [[a - .28, 8], [a - .18, 11], [a + .18, 11], [a + .28, 8]]
                                    for (let j = 0; j < points.length; ++j) {
                                        const x = Math.cos(points[j][0]) * points[j][1], y = Math.sin(points[j][0]) * points[j][1]
                                        if (i === 0 && j === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                                    }
                                }
                                ctx.closePath(); ctx.stroke(); ctx.beginPath(); ctx.arc(0, 0, 3.5, 0, Math.PI * 2); ctx.stroke()
                            }
                        }
                        horizontalPadding: 9
                        Accessible.name: "音频设置"
                        onClicked: { audioLoader.active = true; audioLoader.item.open() }
                        ToolTip.visible: hovered
                        ToolTip.text: "音频设置"
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Panel {
                    title: "房间"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 280
                    ColumnLayout {
                        anchors.fill: parent
                        ListView {
                            id: rooms
                            objectName: "voiceRooms"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 220
                            model: controller.rooms
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar {}
                            delegate: ItemDelegate {
                                id: room
                                font.pixelSize: 16
                                contentItem: Text { text: room.text; font: room.font; color: page.ink; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
                                background: Rectangle { radius: 4; color: room.highlighted || room.hovered ? page.selectedColor : "transparent"; Behavior on color { enabled: !page.themeTransitioning; ColorAnimation { duration: 120 } } }
                                required property var modelData
                                width: rooms.width
                                text: modelData.name + "  (" + modelData.members + "/" + modelData.capacity + ")"
                                highlighted: page.selectedRoom === modelData.id
                                onClicked: page.selectedRoom = modelData.id
                                onDoubleClicked: controller.joinRoom(modelData.id)
                            }
                            Label { anchors.centerIn: parent; visible: controller.rooms.length === 0; text: controller.connected ? "暂无房间，可创建一个房间。" : "连接后显示房间"; wrapMode: Text.Wrap; width: parent.width; horizontalAlignment: Text.AlignHCenter }
                        }
                        RowLayout {
                            VoiceField { id: roomName; objectName: "voiceRoomName"; placeholderText: "新房间名称"; maximumLength: 64; Layout.fillWidth: true; enabled: controller.connected && !controller.inRoom && !controller.busy; onAccepted: controller.createRoom(text) }
                            VoiceButton { objectName: "voiceCreateRoom"; text: "创建"; enabled: controller.connected && !controller.inRoom && !controller.busy; onClicked: controller.createRoom(roomName.text) }
                        }
                        RowLayout {
                            VoiceButton { objectName: "voiceJoinRoom"; text: "加入"; enabled: controller.connected && !controller.inRoom && !controller.busy && page.selectedRoom !== 0; onClicked: controller.joinRoom(page.selectedRoom) }
                            VoiceButton { objectName: "voiceLeaveRoom"; text: "退出"; enabled: controller.connected && controller.inRoom && !controller.busy; onClicked: controller.leaveRoom() }
                        }
                    }
                }
                Panel {
                    title: "成员"
                    fillColor: page.memberColor
                    Layout.fillWidth: true
                    Layout.preferredWidth: 240
                    Layout.fillHeight: true
                    ColumnLayout {
                        anchors.fill: parent
                        Label { text: controller.currentRoom; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        ListView {
                            objectName: "voiceMembers"
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            model: controller.members
                            ScrollBar.vertical: ScrollBar {}
                            delegate: ItemDelegate {
                                id: member
                                required property var modelData
                                width: ListView.view.width
                                font.pixelSize: 16
                                contentItem: Text { text: member.modelData.displayName + (member.modelData.self ? "（自己）" : "") + (member.modelData.muted ? "（静音）" : ""); font: member.font; color: page.ink; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter }
                                background: Rectangle { radius: 4; color: member.hovered ? page.selectedColor : "transparent" }
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                TapHandler {
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onTapped: (eventPoint, button) => {
                                        const popup = memberLoader.item || memberLoader.activate()
                                        popup.memberId = member.modelData.id
                                        const position = member.mapToItem(popup.parent, eventPoint.position.x, eventPoint.position.y)
                                        popup.x = Math.max(0, Math.min(popup.parent.width - popup.width, position.x))
                                        popup.y = Math.max(0, Math.min(popup.parent.height - popup.height, position.y))
                                        popup.open()
                                    }
                                }
                            }
                        }
                    }
                }
            }
            Label { objectName: "voiceStatus"; text: controller.status; wrapMode: Text.Wrap; Layout.fillWidth: true; Layout.leftMargin: 20; Layout.rightMargin: 20; Layout.bottomMargin: 20 }
        }
    }
    Loader {
        id: memberLoader
        active: false
        function activate() { active = true; return item }
        sourceComponent: Popup {
            id: memberPopup
            objectName: "voiceMemberPopup"
            property double memberId: 0
            readonly property var memberData: {
                for (const member of controller.members) if (member.id === memberId) return member
                return null
            }
            onMemberDataChanged: if (!memberData) close()
            onOpened: remarkField.text = memberData ? memberData.remark : ""
            parent: Overlay.overlay
            width: Math.min(300, parent.width - 40)
            padding: 16
            focus: true
            background: Rectangle { color: page.popupColor; border.color: page.edgeColor; radius: 7 }
            enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 180 } }
            exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 140 } }
            contentItem: ColumnLayout {
                Label { text: memberPopup.memberData ? memberPopup.memberData.displayName : ""; font.pixelSize: 16; elide: Text.ElideRight; Layout.fillWidth: true }
                Label { text: memberPopup.memberData ? memberPopup.memberData.ztid : ""; visible: text.length > 0; Layout.fillWidth: true; elide: Text.ElideRight }
                Label { text: memberPopup.memberData && memberPopup.memberData.self ? "这是你自己" : "成员音量" }
                RowLayout {
                    visible: memberPopup.memberData !== null && !memberPopup.memberData.self
                    Slider { objectName: "voiceMemberVolume"; from: 0; to: 200; value: memberPopup.memberData ? memberPopup.memberData.volume : 100; onMoved: controller.setMemberVolume(memberPopup.memberId, Math.round(value)); Layout.fillWidth: true }
                    Label { text: (memberPopup.memberData ? memberPopup.memberData.volume : 100) + "%"; Layout.preferredWidth: 48 }
                }
                ColumnLayout {
                    visible: memberPopup.memberData !== null && !memberPopup.memberData.self
                    Layout.fillWidth: true
                    Label { text: "本地备注" }
                    VoiceField { id: remarkField; objectName: "voiceMemberRemark"; placeholderText: "留空恢复服务器昵称"; maximumLength: 64; enabled: memberPopup.memberData ? memberPopup.memberData.canRemark : false; Layout.fillWidth: true; onAccepted: { controller.setMemberRemark(memberPopup.memberId, text); memberPopup.close() } }
                    Label { text: "无法设置备注：需要服务器提供 ZeroTier ID，并允许保存插件数据。"; visible: memberPopup.memberData !== null && !memberPopup.memberData.canRemark; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    RowLayout {
                        Layout.alignment: Qt.AlignRight
                        VoiceButton { objectName: "voiceSaveMemberRemark"; text: "保存备注"; enabled: remarkField.enabled; onClicked: { controller.setMemberRemark(memberPopup.memberId, remarkField.text); memberPopup.close() } }
                    }
                }
            }
        }
    }
    Loader {
        id: audioLoader
        active: false
        sourceComponent: Popup {
            id: audioPopup
            objectName: "voiceAudioPopup"
            parent: Overlay.overlay
            width: Math.min(440, parent.width - 40)
            height: Math.min(audioContent.implicitHeight + 40, parent.height - 40)
            x: Math.max(20, Math.min(parent.width - width - 20, audioGear.mapToItem(parent, 0, 0).x + audioGear.width - width))
            y: Math.max(20, Math.min(parent.height - height - 20, audioGear.mapToItem(parent, 0, 0).y + audioGear.height + 8))
            padding: 20
            focus: true
            onAboutToHide: controller.setMicrophoneTesting(false)
            background: Rectangle { color: page.popupColor; border.color: page.edgeColor; radius: 8 }
            enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 180; easing.type: Easing.OutCubic } }
            exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 140 } }
            BoundedScroll {
                id: audioScroll
                anchors.fill: parent
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    id: audioContent
                    width: audioScroll.availableWidth
                    spacing: 14
                    Label { text: "音频设置"; font.pixelSize: 18; font.bold: true }
                    VoiceButton { objectName: "voiceTestMicrophone"; text: controller.microphoneTesting ? "停止测试" : "麦克风测试"; onClicked: controller.setMicrophoneTesting(!controller.microphoneTesting); Layout.fillWidth: true }
                    Label { text: "麦克风" }
                    AudioChoice { objectName: "voiceInputDevice"; model: controller.inputs; selectedValue: controller.input; onActivated: controller.input = currentValue; Layout.fillWidth: true }
                    Label { text: "输出设备" }
                    AudioChoice { objectName: "voiceOutputDevice"; model: controller.outputs; selectedValue: controller.output; onActivated: controller.output = currentValue; Layout.fillWidth: true }
                    CheckBox { text: "麦克风静音"; checked: controller.muted; onToggled: controller.muted = checked }
                    CheckBox { text: "耳机静音"; checked: controller.deafened; onToggled: controller.deafened = checked }
                    RowLayout {
                        Label { text: "输入音量" }
                        Slider { objectName: "voiceInputVolume"; from: 0; to: 200; value: controller.inputVolume; onMoved: controller.inputVolume=Math.round(value); Layout.fillWidth: true }
                        Label { text: controller.inputVolume+"%"; Layout.preferredWidth: 48 }
                    }
                    RowLayout {
                        Label { text: "总输出" }
                        Slider { from: 0; to: 200; value: controller.volume; onMoved: controller.volume = Math.round(value); Layout.fillWidth: true }
                        Label { text: controller.volume + "%"; Layout.preferredWidth: 40 }
                    }
                    VoiceButton { text: "关闭"; Layout.alignment: Qt.AlignRight; onClicked: audioPopup.close() }
                }
            }
        }
    }
}
