#include "VoiceSession.h"
#include <Protocol.h>
#include <ztprotocol/WireProtocol.h>
#include <QNetworkDatagram>
#include <QNetworkProxy>
#include <QJsonDocument>
#include <QSignalBlocker>
#include <limits>
namespace vusiif::ztgui::voice_chat {
VoiceSession::VoiceSession(std::shared_ptr<AudioDeviceCache> cache, QObject *parent) : QObject(parent), tcp(new QTcpSocket(this)), udp(new QUdpSocket(this)),
    heartbeat(new QTimer(this)), registration(new QTimer(this)), watchdog(new QTimer(this)), audio(new VoiceAudio(cache->audio.get(),this)), deviceCache(std::move(cache))
{
    // Voice transport is a direct TCP/UDP connection over the selected network.
    tcp->setProxy(QNetworkProxy::NoProxy);
    udp->setProxy(QNetworkProxy::NoProxy);
    tcp->setReadBufferSize(MaxControlBytes + 160);
    heartbeat->setInterval(5000); registration->setInterval(1000);
    watchdog->setInterval(15000); watchdog->setSingleShot(true);
    connect(tcp, &QTcpSocket::connected, this, [this] { request("hello", {{"nickname", nickname},{"ztid",ztid}}); });
    connect(tcp, &QTcpSocket::readyRead, this, &VoiceSession::read);
    connect(tcp, &QTcpSocket::disconnected, this, [this] { close(); emit notice(QStringLiteral("连接已断开，请手动重连")); });
    connect(tcp, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        const auto error = tcp->errorString(); close(); emit notice(QStringLiteral("连接失败：%1").arg(error));
    });
    connect(heartbeat, &QTimer::timeout, this, [this] { if (ready) request("ping"); });
    connect(watchdog, &QTimer::timeout, this, [this] { close(); emit notice(QStringLiteral("服务器响应超时，请手动重连")); });
    connect(registration, &QTimer::timeout, this, &VoiceSession::sendRegistration);
    connect(udp, &QUdpSocket::readyRead, this, &VoiceSession::datagrams);
    audio->notice=[this](const QString &message) { emit notice(message); };
    audio->encoded=[this](const QByteArray &data) {
        if(microphoneTesting) audio->receive(testSender(),testSequence++,data);
        else if (udpReady && room && !muted)
            udp->writeDatagram(protocol::encode("voice-chat", encodePacket({Audio, token, session, sequence++, data})), tcp->peerAddress(), udpPort);
    };
}
void VoiceSession::refreshDevices() {
    deviceCache->refresh(this,[this] { emit devicesRefreshed(); });
}
void VoiceSession::open(const QString &address, quint16 port, const QString &name, const QString &identity)
{
    close();
    const auto host = address.trimmed(); nickname = name.trimmed(); ztid=identity;
    if (host.isEmpty() || nickname.isEmpty()) { emit notice(QStringLiteral("请填写服务器地址和昵称")); return; }
    emit busyChanged(true); emit notice(QStringLiteral("正在连接…"));
    watchdog->start(); tcp->connectToHost(host, port);
}
void VoiceSession::close()
{
    heartbeat->stop(); watchdog->stop(); clearRoom();
    // Avoid recursive close via the synchronous disconnected signal from abort().
    const QSignalBlocker block(tcp);
    tcp->abort(); udp->close(); incoming.clear(); pending.clear();
    ready = false; session = 0; roomPending = false;
    emit busyChanged(false); emit connectionChanged(false); emit roomsChanged({});
}
void VoiceSession::request(const QString &operation, const QJsonObject &data)
{
    if (tcp->state() != QAbstractSocket::ConnectedState) return;
    if (tcp->bytesToWrite() > 256 * 1024 || pending.size() > 64) {
        close(); emit notice(QStringLiteral("服务器响应积压，连接已关闭")); return;
    }
    const int id = nextRequest++;
    if (nextRequest == std::numeric_limits<int>::max()) nextRequest = 1;
    pending.insert(id, operation);
    tcp->write(protocol::frame("voice-chat", QJsonDocument(QJsonObject{{"v", ProtocolVersion}, {"id", id}, {"op", operation}, {"data", data}}).toJson(QJsonDocument::Compact)));
}
void VoiceSession::create(const QString &name) {
    if (!ready || room || roomPending) return;
    roomPending = true; emit busyChanged(true); request("create", {{"name", name}});
}
void VoiceSession::join(quint32 id) {
    if (!ready || room || roomPending) return;
    roomPending = true; emit busyChanged(true); request("join", {{"room", double(id)}});
}
void VoiceSession::leave() {
    if (!ready || !room || roomPending) return;
    roomPending = true; emit busyChanged(true);
    // Stop capture immediately rather than waiting for the server response.
    clearRoom(); request("leave");
}
void VoiceSession::configure(const QByteArray &input, const QByteArray &output, bool allowed) {
    inputId = input; outputId = output; recordAllowed = allowed;
    if (microphoneTesting || (room && udpReady)) {
        audio->start(inputId, outputId, recordAllowed); audio->setMuted(muted && !microphoneTesting);
        audio->setDeafened(deafened && !microphoneTesting); retainMembers();
    }
}
void VoiceSession::testMicrophone(bool enabled) {
    if(microphoneTesting==enabled) return;
    microphoneTesting=enabled; testSequence=0;
    // Restart/stop flushes buffered loopback audio immediately on dismissal.
    if(enabled || (room && udpReady)) configure(inputId,outputId,recordAllowed);
    else audio->stop();
    audio->setMuted(muted && !enabled); audio->setDeafened(deafened && !enabled);
    retainMembers(); emit microphoneTestingChanged(enabled);
}
void VoiceSession::retainMembers() {
    auto retained=members;
    if(microphoneTesting) retained.insert(testSender());
    audio->retain(retained);
}
void VoiceSession::mute(bool value) {
    muted = value; audio->setMuted(value && !microphoneTesting);
    if (ready && room) request("mute", {{"muted", value}});
}
void VoiceSession::clearRoom() {
    if(microphoneTesting) { microphoneTesting=false; emit microphoneTestingChanged(false); }
    audio->stop(); audio->clearMemberVolumes(); registration->stop(); token.clear(); members.clear();
    room = 0; udpReady = false; sequence = 0; emit roomChanged({});
}
void VoiceSession::updateRoom(const QJsonObject &value) {
    if (quint32(value.value("id").toDouble()) != room) return;
    members.clear();
    for (const auto &entry : value.value("members").toArray()) {
        const auto id = quint32(entry.toObject().value("id").toDouble());
        if (id != session) members.insert(id);
    }
    retainMembers();
    auto snapshot=value; auto rows=value["members"].toArray();
    for(qsizetype i=0;i<rows.size();++i) { auto row=rows[i].toObject(); row["self"]=quint32(row["id"].toDouble())==session; rows[i]=row; }
    snapshot["members"]=rows; emit roomChanged(snapshot);
}
void VoiceSession::read()
{
    incoming += tcp->readAll();
    for (;;) {
        protocol::Packet packet;
        const auto result = protocol::takeFrame(incoming,packet);
        if (result == protocol::ReadResult::Incomplete) break;
        if (result == protocol::ReadResult::Invalid || packet.plugin != "voice-chat" || packet.payload.size() > MaxControlBytes) {
            close(); emit notice(QStringLiteral("服务器协议无效")); return;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(packet.payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) { close(); emit notice(QStringLiteral("服务器协议无效")); return; }
        const auto message = document.object();
        if (message.value("v").toInt() != ProtocolVersion) { close(); emit notice(QStringLiteral("服务器协议版本不兼容")); return; }
        watchdog->start();
        if (message.contains("event")) {
            const auto event = message.value("event").toString();
            if (event == "rooms") emit roomsChanged(message.value("data").toArray());
            else if (event == "members") updateRoom(message.value("data").toObject());
            continue;
        }
        const auto operation = pending.take(message.value("id").toInt());
        if (operation.isEmpty()) continue;
        if (operation == "join" || operation == "create" || operation == "leave") { roomPending = false; emit busyChanged(false); }
        if (!message.value("ok").toBool()) {
            const auto code = message.value("error").toString();
            const QMap<QString, QString> errors{{"room_full", "房间已满"}, {"room_not_found", "房间已不存在"},
                {"room_limit", "服务器房间数量已达上限"}, {"invalid_room_name", "房间名称不能为空或超过64字"},
                {"protocol_mismatch", "协议版本不兼容"}, {"invalid_nickname", "昵称无效"}};
            if (operation == "hello") close();
            emit notice(errors.value(code, QStringLiteral("操作失败：%1").arg(code))); continue;
        }
        const auto data = message.value("data");
        if (operation == "hello") {
            const auto object = data.toObject(); session = quint32(object.value("session").toDouble());
            udpPort = quint16(object.value("udpPort").toInt());
            if (!session || !udpPort || !udp->bind(tcp->peerAddress().protocol() == QAbstractSocket::IPv6Protocol ? QHostAddress::AnyIPv6 : QHostAddress::AnyIPv4, 0)) {
                close(); emit notice(QStringLiteral("语音 UDP 初始化失败")); return;
            }
            ready = true; heartbeat->start(); emit busyChanged(false); emit connectionChanged(true);
            emit roomsChanged(object.value("rooms").toArray()); emit notice(QStringLiteral("已连接，请创建或加入房间"));
        } else if (operation == "create" || operation == "join") {
            const auto object = data.toObject(); token = QByteArray::fromHex(object.value("token").toString().toLatin1());
            room = quint32(object.value("room").toObject().value("id").toDouble());
            if (!room || token.size() != 16) { close(); emit notice(QStringLiteral("服务器房间响应无效")); return; }
            updateRoom(object.value("room").toObject());
            if (muted) request("mute", {{"muted", true}});
            registrationAttempts = 0; sendRegistration(); registration->start();
            emit notice(QStringLiteral("已入房，正在验证语音通道…"));
        } else if (operation == "rooms") emit roomsChanged(data.toArray());
        else if (operation == "leave") emit notice(QStringLiteral("已退出房间"));
    }
    if (incoming.size() > MaxControlBytes + 160) { close(); emit notice(QStringLiteral("服务器消息过长")); }
}
void VoiceSession::sendRegistration() {
    if (!room || udpReady) return;
    if (++registrationAttempts > 5) {
        registration->stop();
        emit notice(QStringLiteral("语音 UDP 通道不可达，请检查 UDP %1 端口后退房重试").arg(udpPort));
        return;
    }
    udp->writeDatagram(protocol::encode("voice-chat", encodePacket({Register, token, session, 0, {}})), tcp->peerAddress(), udpPort);
}
void VoiceSession::datagrams() {
    while (udp->hasPendingDatagrams()) {
        const auto datagram = udp->receiveDatagram(protocol::MaxUdpBytes + 1);
        Packet p; protocol::Packet packet;
        if (!room || datagram.senderAddress() != tcp->peerAddress() || datagram.senderPort() != udpPort
            || !protocol::decode(datagram.data(), packet) || packet.plugin != "voice-chat" || !decodePacket(packet.payload, p) || p.token != token) continue;
        if (p.type == Registered && p.sender == session && !udpReady) {
            udpReady = true; registration->stop(); emit notice(QStringLiteral("语音通道已就绪"));
            configure(inputId,outputId,recordAllowed);
        } else if (p.type == Audio && udpReady && members.count(p.sender)) audio->receive(p.sender, p.sequence, p.payload);
    }
}
}
