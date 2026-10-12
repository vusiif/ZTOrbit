#include "VoicePage.h"
#include "VoiceSession.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaDevices>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QCoreApplication>
#include <QApplication>
#include <QPermissions>
#include <QScrollArea>

namespace vusiif::ztgui::voice_chat {
VoicePage::VoicePage(QWidget *parent, std::shared_ptr<AudioDeviceCache> cache)
    : QWidget(parent), session(new VoiceSession(cache)), deviceCache(std::move(cache)), devices(new QMediaDevices(this))
{
    setObjectName("voiceChatPage");
    auto *outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget; scroll->setWidget(content); outer->addWidget(scroll);
    auto *layout = new QVBoxLayout(content); layout->setContentsMargins(20, 20, 20, 20); layout->setSpacing(14);
    auto *heading = new QLabel(QStringLiteral("语音聊天")); heading->setObjectName("sectionTitle"); layout->addWidget(heading);
    layout->addWidget(new QLabel(QStringLiteral("填写语音服务器地址，连接后选择房间。")));
    auto *connectionBox = new QGroupBox(QStringLiteral("服务器"));
    auto *form = new QFormLayout(connectionBox);
    address = new QLineEdit; address->setObjectName("voiceServerAddress"); address->setPlaceholderText(QStringLiteral("服务器 IP 或域名"));
    nickname = new QLineEdit; nickname->setObjectName("voiceNickname"); nickname->setMaxLength(64);
    port = new QSpinBox; port->setObjectName("voiceServerPort"); port->setRange(1, 65535); port->setValue(DefaultPort);
    connection = new QPushButton(QStringLiteral("连接")); connection->setObjectName("voiceConnect");
    auto *serverRow = new QHBoxLayout; serverRow->addWidget(address, 1); serverRow->addWidget(port); serverRow->addWidget(connection);
    form->addRow(QStringLiteral("地址 / 端口"), serverRow); form->addRow(QStringLiteral("昵称"), nickname); layout->addWidget(connectionBox);
    auto *roomLayout = new QHBoxLayout;
    auto *roomBox = new QGroupBox(QStringLiteral("房间")); auto *roomContent = new QVBoxLayout(roomBox);
    rooms = new QListWidget; rooms->setObjectName("voiceRooms"); roomContent->addWidget(rooms);
    auto *newRow = new QHBoxLayout; roomName = new QLineEdit; roomName->setPlaceholderText(QStringLiteral("新房间名称")); roomName->setMaxLength(64);
    roomName->setObjectName("voiceRoomName");
    createRoom = new QPushButton(QStringLiteral("创建")); createRoom->setObjectName("voiceCreateRoom"); newRow->addWidget(roomName, 1); newRow->addWidget(createRoom); roomContent->addLayout(newRow);
    auto *roomActions = new QHBoxLayout; joinRoom = new QPushButton(QStringLiteral("加入")); leaveRoom = new QPushButton(QStringLiteral("退出"));
    joinRoom->setObjectName("voiceJoinRoom"); leaveRoom->setObjectName("voiceLeaveRoom");
    roomActions->addWidget(joinRoom); roomActions->addWidget(leaveRoom); roomContent->addLayout(roomActions); roomLayout->addWidget(roomBox, 1);
    auto *memberBox = new QGroupBox(QStringLiteral("成员")); auto *memberContent = new QVBoxLayout(memberBox);
    currentRoom = new QLabel(QStringLiteral("尚未加入房间")); members = new QListWidget; members->setObjectName("voiceMembers");
    memberContent->addWidget(currentRoom); memberContent->addWidget(members); roomLayout->addWidget(memberBox, 1); layout->addLayout(roomLayout, 1);
    auto *audioBox = new QGroupBox(QStringLiteral("音频")); auto *audioForm = new QFormLayout(audioBox);
    inputs = new QComboBox; outputs = new QComboBox; audioForm->addRow(QStringLiteral("麦克风"), inputs); audioForm->addRow(QStringLiteral("输出设备"), outputs);
    micMute = new QCheckBox(QStringLiteral("麦克风静音")); earMute = new QCheckBox(QStringLiteral("耳机静音"));
    volume = new QSlider(Qt::Horizontal); volume->setRange(0, 100); volume->setValue(80);
    auto *audioActions = new QHBoxLayout; audioActions->addWidget(micMute); audioActions->addWidget(earMute); audioActions->addWidget(new QLabel(QStringLiteral("音量"))); audioActions->addWidget(volume, 1);
    audioForm->addRow(audioActions); layout->addWidget(audioBox);
    status = new QLabel(QStringLiteral("未连接")); status->setWordWrap(true); status->setObjectName("voiceStatus"); layout->addWidget(status);
    QSettings settings; address->setText(settings.value("voice-chat/address").toString()); nickname->setText(settings.value("voice-chat/nickname").toString());
    port->setValue(settings.value("voice-chat/port", DefaultPort).toInt()); volume->setValue(settings.value("voice-chat/volume", 80).toInt());
    micMute->setChecked(settings.value("voice-chat/muted", false).toBool()); earMute->setChecked(settings.value("voice-chat/deafened", false).toBool());
    refreshDevices();
    session->moveToThread(&thread);
    connect(&thread, &QThread::finished, session, &QObject::deleteLater);
    connect(session, &VoiceSession::notice, this, [this](const QString &message) { status->setText(message); });
    connect(session, &VoiceSession::connectionChanged, this, [this](bool value) { connected = value; updateActions(); });
    connect(session, &VoiceSession::busyChanged, this, [this](bool value) { busy = value; updateActions(); });
    connect(session, &VoiceSession::devicesRefreshed, this, [this] {
        deviceRefreshPending = false; refreshDevices(); monitorDevices(); configureAudio();
    });
    connect(session, &VoiceSession::roomsChanged, this, [this](const QJsonArray &list) {
        const auto selected = rooms->currentItem() ? rooms->currentItem()->data(Qt::UserRole).toUInt() : 0;
        rooms->clear();
        for (const auto &value : list) {
            const auto object = value.toObject();
            auto *item = new QListWidgetItem(QStringLiteral("%1  (%2/%3)").arg(object.value("name").toString()).arg(object.value("members").toInt()).arg(object.value("capacity").toInt()), rooms);
            item->setData(Qt::UserRole, quint32(object.value("id").toDouble()));
            if (item->data(Qt::UserRole).toUInt() == selected) rooms->setCurrentItem(item);
        }
        updateActions();
    });
    connect(session, &VoiceSession::roomChanged, this, [this](const QJsonObject &value) {
        inRoom = !value.isEmpty(); currentRoom->setText(inRoom ? value.value("name").toString() : QStringLiteral("尚未加入房间")); members->clear();
        for (const auto &member : value.value("members").toArray()) {
            const auto object = member.toObject(); members->addItem(object.value("nickname").toString() + (object.value("muted").toBool() ? QStringLiteral("（静音）") : QString()));
        }
        updateActions();
    });
    connect(connection, &QPushButton::clicked, this, [this] {
        if (connected || busy) { QMetaObject::invokeMethod(session, [this] { session->close(); }); status->setText(QStringLiteral("已断开")); return; }
        if (address->text().trimmed().isEmpty() || nickname->text().trimmed().isEmpty()) { status->setText(QStringLiteral("请填写服务器地址和昵称")); return; }
        save(); const auto host = address->text(), name = nickname->text(); const auto servicePort = quint16(port->value());
        busy = true; updateActions();
        QMetaObject::invokeMethod(session, [worker = session, host, name, servicePort] { worker->open(host, servicePort, name); });
    });
    connect(createRoom, &QPushButton::clicked, this, [this] {
        if (roomName->text().trimmed().isEmpty()) { status->setText(QStringLiteral("请填写房间名称")); return; }
        const auto name = roomName->text();
        withPermission([this, name] { busy = true; updateActions(); QMetaObject::invokeMethod(session, [worker = session, name] { worker->create(name); }); });
    });
    connect(joinRoom, &QPushButton::clicked, this, [this] {
        if (!rooms->currentItem()) return; const auto id = rooms->currentItem()->data(Qt::UserRole).toUInt();
        withPermission([this, id] { busy = true; updateActions(); QMetaObject::invokeMethod(session, [worker = session, id] { worker->join(id); }); });
    });
    connect(leaveRoom, &QPushButton::clicked, this, [this] { busy = true; updateActions(); QMetaObject::invokeMethod(session, [worker = session] { worker->leave(); }); });
    connect(rooms, &QListWidget::currentRowChanged, this, [this] { updateActions(); });
    connect(inputs, &QComboBox::currentIndexChanged, this, [this] { configureAudio(); save(); });
    connect(outputs, &QComboBox::currentIndexChanged, this, [this] { configureAudio(); save(); });
    connect(micMute, &QCheckBox::toggled, this, [this](bool value) { save(); QMetaObject::invokeMethod(session, [worker = session, value] { worker->mute(value); }); });
    connect(earMute, &QCheckBox::toggled, this, [this](bool value) { save(); QMetaObject::invokeMethod(session, [worker = session, value] { worker->deafen(value); }); });
    connect(volume, &QSlider::valueChanged, this, [this](int value) { save(); QMetaObject::invokeMethod(session, [worker = session, value] { worker->volume(value); }); });
    thread.start(); configureAudio();
    if (deviceCache->snapshot().ready) monitorDevices();
    requestDeviceRefresh();
    const bool muteValue = micMute->isChecked(), deafenValue = earMute->isChecked(); const int volumeValue = volume->value();
    QMetaObject::invokeMethod(session, [worker = session, muteValue, deafenValue, volumeValue] { worker->mute(muteValue); worker->deafen(deafenValue); worker->volume(volumeValue); });
    updateActions();
}
VoicePage::~VoicePage() {
    save();
    QMetaObject::invokeMethod(session, [worker = session] { worker->close(); }, Qt::BlockingQueuedConnection);
    thread.quit(); thread.wait();
}
void VoicePage::updateActions() {
    connection->setText(connected ? QStringLiteral("断开") : busy ? QStringLiteral("取消") : QStringLiteral("连接"));
    address->setEnabled(!connected && !busy); port->setEnabled(!connected && !busy); nickname->setEnabled(!connected && !busy);
    createRoom->setEnabled(connected && !inRoom && !busy); joinRoom->setEnabled(connected && !inRoom && !busy && rooms->currentItem());
    leaveRoom->setEnabled(connected && inRoom && !busy); roomName->setEnabled(connected && !inRoom && !busy);
}
void VoicePage::refreshDevices() {
    QSettings settings;
    auto fill = [&](QComboBox *box, const QList<QAudioDevice> &list, const QString &key) {
        const auto selected = box->count() ? box->currentData().toByteArray() : settings.value(key).toByteArray();
        const QSignalBlocker block(box); box->clear(); box->addItem(QStringLiteral("系统默认"), QByteArray{});
        for (const auto &device : list) box->addItem(device.description(), device.id());
        const int index = box->findData(selected); box->setCurrentIndex(index < 0 ? 0 : index);
    };
    const auto snapshot = deviceCache->snapshot();
    fill(inputs, snapshot.inputs, "voice-chat/input"); fill(outputs, snapshot.outputs, "voice-chat/output");
}
void VoicePage::requestDeviceRefresh() {
    if (deviceRefreshPending) return;
    deviceRefreshPending = true;
    QMetaObject::invokeMethod(session, [worker = session] { worker->refreshDevices(); });
}
void VoicePage::monitorDevices() {
    if (monitoringDevices) return;
    monitoringDevices = true;
    connect(devices, &QMediaDevices::audioInputsChanged, this, [this] {
        requestDeviceRefresh(); status->setText(QStringLiteral("输入设备已变化，请检查设备选择"));
    });
    connect(devices, &QMediaDevices::audioOutputsChanged, this, [this] {
        requestDeviceRefresh(); status->setText(QStringLiteral("输出设备已变化，请检查设备选择"));
    });
}
void VoicePage::configureAudio() {
    const auto input = inputs->currentData().toByteArray(), output = outputs->currentData().toByteArray(); const bool allowed = recordAllowed;
    QMetaObject::invokeMethod(session, [worker = session, input, output, allowed] { worker->configure(input, output, allowed); });
}
void VoicePage::save() {
    QSettings settings;
    settings.setValue("voice-chat/address", address->text().trimmed()); settings.setValue("voice-chat/port", port->value());
    settings.setValue("voice-chat/nickname", nickname->text().trimmed()); settings.setValue("voice-chat/input", inputs->currentData()); settings.setValue("voice-chat/output", outputs->currentData());
    settings.setValue("voice-chat/volume", volume->value()); settings.setValue("voice-chat/muted", micMute->isChecked()); settings.setValue("voice-chat/deafened", earMute->isChecked());
}
void VoicePage::withPermission(std::function<void()> action) {
#ifdef Q_OS_MACOS
    const QMicrophonePermission permission;
    const auto state = qApp->checkPermission(permission);
    if (state == Qt::PermissionStatus::Undetermined) {
        qApp->requestPermission(permission, this, [this, action](const QPermission &result) {
            recordAllowed = result.status() == Qt::PermissionStatus::Granted; configureAudio(); action();
        }); return;
    }
    recordAllowed = state == Qt::PermissionStatus::Granted;
#endif
    configureAudio(); action();
}
}
