import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Desktop-share page. Every state shown here comes from the injected controller
// (phase/phaseLabel/statusText/errorText/nextAction and the can* capabilities);
// the page never infers a remote/room state on its own. Opening the page only
// enumerates sources and reads the saved address/profile. All visuals are plain
// Qt Quick items so the host's default CPU (software) rendering stays supported,
// and all sizes scale with the application font and the host window.
Pane {
    id: root
    required property var controller
    padding: 16
    // One scalable unit derived from the effective UI font, so the page follows
    // the host's high-DPI scaling and window resizing without fixed pixel sizes.
    readonly property real unit: Math.max(12, root.font.pixelSize > 0
                                            ? root.font.pixelSize
                                            : Math.round(Qt.application.font.pointSize * 1.34))

    function phaseBadgeColor() {
        if (root.controller.phase === "failed" || root.controller.phase === "stopFailed")
            return root.palette.highlight
        if (root.controller.phase === "mediaInterrupted" || root.controller.phase === "waitingNewFrame")
            return root.palette.mid
        if (root.controller.phase === "sharing" || root.controller.phase === "viewing"
                || root.controller.phase === "previewing")
            return root.palette.highlight
        return root.palette.mid
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                objectName: "desktopShareTitle"
                text: "桌面共享"
                font.pixelSize: Math.round(root.unit * 1.7)
                font.bold: true
            }
            Item { Layout.fillWidth: true }
            BusyIndicator {
                objectName: "desktopShareBusy"
                Layout.preferredWidth: Math.round(root.unit * 1.6)
                Layout.preferredHeight: Layout.preferredWidth
                running: root.controller.busy
                visible: running
            }
            Rectangle {
                Layout.preferredWidth: phaseLabel.implicitWidth + 18
                Layout.preferredHeight: phaseLabel.implicitHeight + 8
                radius: 4
                color: root.phaseBadgeColor()
                Label {
                    id: phaseLabel
                    objectName: "desktopSharePhaseLabel"
                    anchors.centerIn: parent
                    text: root.controller.phaseLabel
                    color: root.palette.highlightedText
                    font.bold: true
                }
                Accessible.name: "生命周期状态 " + root.controller.phaseLabel
            }
            Label {
                objectName: "desktopSharePhase"
                visible: false
                text: root.controller.phase
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label { text: "服务器"; Accessible.name: "服务器地址标签" }
            TextField {
                id: serverField
                objectName: "desktopShareServer"
                Layout.fillWidth: true
                Layout.minimumWidth: 160
                Layout.maximumWidth: 420
                placeholderText: "地址[:端口]，默认端口 18100"
                enabled: !root.controller.connected && !root.controller.busy
                text: root.controller.serverAddress
                selectByMouse: true
                onEditingFinished: root.controller.serverAddress = text
                Accessible.name: "服务器地址"
            }
            Button {
                objectName: "desktopShareConnect"
                text: root.controller.connected ? "断开" : "连接"
                enabled: root.controller.connected ? root.controller.canDisconnect
                                                   : root.controller.canConnect
                onClicked: {
                    if (root.controller.connected) {
                        root.controller.disconnectServer()
                    } else {
                        root.controller.serverAddress = serverField.text
                        root.controller.connectServer()
                    }
                }
                Accessible.name: text
            }
            Button {
                objectName: "desktopShareCancel"
                text: "取消"
                visible: root.controller.canCancel
                enabled: root.controller.canCancel
                onClicked: root.controller.cancelOperation()
                Accessible.name: "取消当前操作"
            }
            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label { text: "来源"; Accessible.name: "共享来源标签" }
            ComboBox {
                id: sourceBox
                objectName: "desktopShareSource"
                Layout.fillWidth: true
                Layout.minimumWidth: 180
                Layout.maximumWidth: 520
                model: root.controller.sources
                textRole: "description"
                valueRole: "value"
                enabled: root.controller.canSelectSource && root.controller.sources.length > 0
                displayText: count === 0 ? "没有可共享的屏幕或窗口"
                                          : currentIndex < 0 ? "请选择屏幕或窗口" : currentText
                onActivated: root.controller.sourceValue = currentValue
                Accessible.name: "共享来源"
                contentItem: Text {
                    leftPadding: 8
                    rightPadding: 26
                    text: sourceBox.displayText
                    font: sourceBox.font
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                    color: sourceBox.enabled ? sourceBox.palette.text : sourceBox.palette.mid
                }

                function syncFromController() {
                    const value = root.controller.sourceValue
                    let index = -1
                    for (let i = 0; i < count; ++i) {
                        if (valueAt(i) === value) {
                            index = i
                            break
                        }
                    }
                    currentIndex = index
                }
                Component.onCompleted: syncFromController()
                Connections {
                    target: root.controller
                    function onChanged() { sourceBox.syncFromController() }
                }
            }

            Button {
                objectName: "desktopShareRefresh"
                text: "刷新来源"
                enabled: root.controller.canRefreshSources
                onClicked: root.controller.refreshSources()
                Accessible.name: "刷新来源"
            }
            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label { text: "视频"; Accessible.name: "视频格式标签" }

            ComboBox {
                id: codecBox
                objectName: "desktopShareCodec"
                Layout.preferredWidth: Math.round(root.unit * 12)
                model: [
                    { text: "文字桌面 · 变化块", value: "bgra-tiles" },
                    { text: "视频 · H.264", value: "h264" }
                ]
                textRole: "text"
                valueRole: "value"
                enabled: root.controller.canSelectVideo
                onActivated: root.controller.codec = currentValue
                Accessible.name: "视频格式"
                function syncFromController() {
                    for (let i = 0; i < count; ++i)
                        if (valueAt(i) === root.controller.codec) { currentIndex = i; break }
                }
                Component.onCompleted: syncFromController()
                Connections {
                    target: root.controller
                    function onChanged() { codecBox.syncFromController() }
                }
            }

            ComboBox {
                id: profileBox
                objectName: "desktopShareProfile"
                Layout.preferredWidth: Math.round(root.unit * 11)
                model: [
                    { text: "720p · 2 Mbps", value: "720p30" },
                    { text: "1080p · 4 Mbps", value: "1080p30" }
                ]
                textRole: "text"
                valueRole: "value"
                enabled: root.controller.canSelectVideo
                onActivated: root.controller.profile = currentValue
                Accessible.name: "视频档位"
                function syncFromController() {
                    const value = root.controller.profile
                    for (let i = 0; i < count; ++i) {
                        if (valueAt(i) === value) {
                            currentIndex = i
                            break
                        }
                    }
                }
                Component.onCompleted: syncFromController()
                Connections {
                    target: root.controller
                    function onChanged() { profileBox.syncFromController() }
                }
            }
            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                objectName: "desktopShareStart"
                text: root.controller.publishing && root.controller.phase === "sharing"
                      ? "共享中"
                      : root.controller.busy && root.controller.pendingOperation === "start"
                        ? "准备中…" : "开始共享"
                enabled: root.controller.canStartShare
                onClicked: root.controller.startSharing()
                Accessible.name: "开始共享"
            }

            Button {
                objectName: "desktopShareStop"
                text: "停止共享"
                enabled: root.controller.canStopShare
                onClicked: root.controller.stopSharing()
                Accessible.name: "停止共享"
            }

            Rectangle {
                Layout.preferredWidth: roomLabel.implicitWidth + 16
                Layout.preferredHeight: roomLabel.implicitHeight + 8
                radius: 4
                color: root.palette.button
                border.color: root.palette.mid
                border.width: 1
                Label {
                    id: roomLabel
                    objectName: "desktopShareRoom"
                    anchors.centerIn: parent
                    text: root.controller.roomCode.length > 0
                          ? ("房间号：" + root.controller.roomCode)
                          : "房间号：—"
                    font.bold: root.controller.roomCode.length > 0
                    color: root.palette.buttonText
                }
            }

            Button {
                objectName: "desktopShareCopy"
                text: "复制房号"
                enabled: root.controller.canCopyRoom
                onClicked: {
                    roomClipboard.text = root.controller.roomCode
                    roomClipboard.selectAll()
                    roomClipboard.copy()
                }
                Accessible.name: "复制房号"
            }
            TextEdit {
                id: roomClipboard
                visible: false
            }

            Item { Layout.fillWidth: true }

            Label {
                objectName: "desktopShareViewers"
                text: "观看人数：" + root.controller.viewerCount
                opacity: 0.85
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label { text: "观看"; Accessible.name: "观看房间标签" }
            TextField {
                id: joinField
                objectName: "desktopShareJoinCode"
                Layout.preferredWidth: Math.round(root.unit * 10)
                placeholderText: "八位房间号"
                maximumLength: 8
                enabled: !root.controller.publishing && !root.controller.busy
                         && !root.controller.watching
                text: root.controller.joinCode
                selectByMouse: true
                onEditingFinished: root.controller.joinCode = text
                Accessible.name: "房间号"
            }
            Button {
                objectName: "desktopShareJoin"
                text: "加入观看"
                enabled: root.controller.canJoin && joinField.text.length === 8
                onClicked: {
                    root.controller.joinCode = joinField.text
                    root.controller.joinRoomExplicit()
                }
                Accessible.name: "加入观看"
            }
            Button {
                objectName: "desktopShareLeave"
                text: "退出观看"
                enabled: root.controller.canLeave
                onClicked: root.controller.leaveRoom()
                Accessible.name: "退出观看"
            }
            Item { Layout.fillWidth: true }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            Label {
                objectName: "desktopShareError"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                visible: text.length > 0
                color: root.palette.highlight
                font.bold: true
                text: root.controller.errorText.length > 0
                      ? ("错误：" + root.controller.errorText) : ""
            }
            Label {
                objectName: "desktopShareNextAction"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                visible: root.controller.nextAction.length > 0
                opacity: 0.9
                text: "下一步：" + root.controller.nextAction
            }
            Label {
                objectName: "desktopShareStatus"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.controller.statusText
            }
            Label {
                objectName: "desktopSharePreferencesError"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                visible: text.length > 0
                color: root.palette.highlight
                text: root.controller.preferencesError
            }
            Label {
                objectName: "desktopShareDetails"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.controller.details
                opacity: 0.75
                visible: text.length > 0
            }
        }

        Frame {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: Math.round(root.unit * 12)
            padding: 0
            background: Rectangle {
                color: "black"
                border.color: root.palette.mid
                border.width: 1
            }

            Item {
                objectName: "desktopSharePreview"
                anchors.fill: parent
                clip: true

                Label {
                    objectName: "desktopSharePreviewHint"
                    z: 2
                    anchors.centerIn: parent
                    width: parent.width * 0.9
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    color: "white"
                    opacity: 0.72
                    visible: !root.controller.previewLive
                    text: root.controller.previewHint
                }

                Rectangle {
                    objectName: "desktopSharePreviewBadge"
                    z: 2
                    visible: root.controller.previewLive
                             && (root.controller.phase === "waitingNewFrame"
                                 || root.controller.phase === "mediaInterrupted")
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.margins: 8
                    width: badgeLabel.implicitWidth + 12
                    height: badgeLabel.implicitHeight + 6
                    radius: 4
                    color: "#99000000"
                    Label {
                        id: badgeLabel
                        objectName: "desktopShareStatusBadge"
                        anchors.centerIn: parent
                        color: "white"
                        font.pixelSize: Math.round(root.unit * 0.85)
                        text: root.controller.phase === "mediaInterrupted"
                              ? "媒体中断 · 保留最后画面" : "等待新画面"
                    }
                }
            }
        }
    }
}
