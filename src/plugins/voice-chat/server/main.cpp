#include "../Protocol.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QJsonArray>
#include <QMap>
#include <QNetworkDatagram>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QDebug>
#include <limits>

using namespace vusiif::ztgui::voice_chat;
namespace vusiif::ztgui::voice_chat {
struct Session {
    QTcpSocket *socket = nullptr;
    quint32 id = 0, room = 0;
    QString nickname;
    QByteArray input, token;
    QHostAddress udpAddress;
    quint16 udpPort = 0;
    bool hello = false, muted = false;
    qint64 lastSeen = 0, rateWindow = 0;
    int controls = 0, packets = 0;
};
struct Room { QString name; };

class Server : public QObject {
public:
    Server(int roomLimit, int memberLimit, int connectionLimit)
        : maxRooms(roomLimit), maxMembers(memberLimit), maxConnections(connectionLimit)
    {
        connect(&tcp, &QTcpServer::newConnection, this, [this] { accept(); });
        connect(&udp, &QUdpSocket::readyRead, this, [this] { datagrams(); });
        connect(&timer, &QTimer::timeout, this, [this] {
            const auto now = QDateTime::currentMSecsSinceEpoch();
            for (auto *socket : sessions.keys())
                if (now - sessions[socket].lastSeen > 15000) socket->abort();
        });
        timer.start(1000);
    }
    bool listen(const QHostAddress &address, quint16 tcpPort, quint16 udpPort)
    {
        if (!tcp.listen(address, tcpPort)) { qCritical() << tcp.errorString(); return false; }
        if (!udp.bind(address, udpPort)) { qCritical() << udp.errorString(); return false; }
        qInfo() << "voice server listening" << address << "TCP" << tcp.serverPort() << "UDP" << udp.localPort();
        return true;
    }
private:
    QTcpServer tcp;
    QUdpSocket udp;
    QTimer timer;
    QMap<QTcpSocket *, Session> sessions;
    QMap<quint32, Room> rooms;
    quint32 nextSession = 1, nextRoom = 1;
    int maxRooms, maxMembers, maxConnections;
    int population(quint32 room) const {
        int count = 0;
        for (const auto &s : sessions) if (s.room == room) ++count;
        return count;
    }
    QJsonArray roomList() const {
        QJsonArray list;
        for (auto i = rooms.cbegin(); i != rooms.cend(); ++i)
            list.append(QJsonObject{{"id", double(i.key())}, {"name", i->name}, {"members", population(i.key())}, {"capacity", maxMembers}});
        return list;
    }
    QJsonObject roomState(quint32 id) const {
        QJsonArray members;
        for (const auto &s : sessions) if (s.room == id)
            members.append(QJsonObject{{"id", double(s.id)}, {"nickname", s.nickname}, {"muted", s.muted}});
        return {{"id", double(id)}, {"name", rooms.value(id).name}, {"members", members}};
    }
    void send(QTcpSocket *socket, const QJsonObject &object) {
        if (socket->bytesToWrite() > 256 * 1024) {
            QTimer::singleShot(0, socket, [socket] { socket->abort(); }); return;
        }
        socket->write(jsonLine(object));
    }
    void changed(quint32 room) {
        const auto list = roomList();
        for (auto *socket : sessions.keys()) {
            const auto &s = sessions[socket];
            if (!s.hello) continue;
            send(socket, {{"v", ProtocolVersion}, {"event", "rooms"}, {"data", list}});
            if (room && s.room == room)
                send(socket, {{"v", ProtocolVersion}, {"event", "members"}, {"data", roomState(room)}});
        }
    }
    void leave(Session &s) {
        const auto oldRoom = s.room;
        s.room = 0; s.token.clear(); s.udpPort = 0; s.udpAddress.clear(); s.muted = false;
        if (oldRoom) {
            if (!population(oldRoom)) rooms.remove(oldRoom);
            qInfo() << "session left room" << s.id << oldRoom;
            changed(oldRoom);
        }
    }
    void accept() {
        while (tcp.hasPendingConnections()) {
            auto *socket = tcp.nextPendingConnection();
            socket->setReadBufferSize(MaxControlBytes + 1);
            if (sessions.size() >= maxConnections || nextSession == 0) { socket->abort(); socket->deleteLater(); continue; }
            Session s; s.socket = socket; s.id = nextSession++;
            s.lastSeen = s.rateWindow = QDateTime::currentMSecsSinceEpoch();
            sessions.insert(socket, s);
            qInfo() << "connected session" << s.id << socket->peerAddress();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                auto it = sessions.find(socket);
                if (it != sessions.end()) { leave(it.value()); sessions.erase(it); }
                socket->deleteLater();
            });
        }
    }
    bool allow(Session &s, bool audio) {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (now - s.rateWindow >= 1000) { s.controls = s.packets = 0; s.rateWindow = now; }
        return audio ? ++s.packets <= 100 : ++s.controls <= 30;
    }
    void read(QTcpSocket *socket) {
        // Disconnection callbacks may erase a session; return immediately after abort.
        auto &s = sessions[socket];
        s.input += socket->readAll();
        while (s.input.contains('\n')) {
            const auto end = s.input.indexOf('\n');
            if (end > MaxControlBytes || !allow(s, false)) { socket->abort(); return; }
            const auto line = s.input.left(end); s.input.remove(0, end + 1);
            QJsonParseError error;
            const auto doc = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject()) { socket->abort(); return; }
            s.lastSeen = QDateTime::currentMSecsSinceEpoch();
            command(s, doc.object());
        }
        if (s.input.size() > MaxControlBytes) socket->abort();
    }
    void command(Session &s, const QJsonObject &request) {
        QJsonObject response{{"v", ProtocolVersion}, {"id", request.value("id")}, {"ok", false}};
        auto fail = [&](const QString &code) { response["error"] = code; send(s.socket, response); };
        if (request.value("v").toInt(-1) != ProtocolVersion) { fail("protocol_mismatch"); return; }
        const auto op = request.value("op").toString();
        const auto data = request.value("data").toObject();
        QJsonValue result;
        if (op == "hello") {
            if (s.hello) { fail("already_connected"); return; }
            const auto name = data.value("nickname").toString().trimmed();
            if (name.isEmpty() || name.size() > 64) { fail("invalid_nickname"); return; }
            s.nickname = name; s.hello = true;
            result = QJsonObject{{"session", double(s.id)}, {"udpPort", udp.localPort()}, {"rooms", roomList()}};
        } else if (!s.hello) { fail("hello_required"); return; }
        else if (op == "ping") result = QJsonObject{};
        else if (op == "rooms") result = roomList();
        else if (op == "leave") { leave(s); result = QJsonObject{}; }
        else if (op == "mute") {
            if (!s.room || !data.value("muted").isBool()) { fail("invalid_state"); return; }
            s.muted = data.value("muted").toBool(); changed(s.room); result = QJsonObject{};
        } else if (op == "create" || op == "join") {
            if (s.room) { fail("already_in_room"); return; }
            quint32 id = 0;
            if (op == "create") {
                const auto name = data.value("name").toString().trimmed();
                if (name.isEmpty() || name.size() > 64) { fail("invalid_room_name"); return; }
                if (rooms.size() >= maxRooms || nextRoom == 0) { fail("room_limit"); return; }
                id = nextRoom++; rooms.insert(id, {name});
            } else {
                const auto number = data.value("room").toDouble(-1);
                if (number < 1 || number > std::numeric_limits<quint32>::max() || number != quint32(number)) { fail("room_not_found"); return; }
                id = quint32(number);
                if (!rooms.contains(id)) { fail("room_not_found"); return; }
                if (population(id) >= maxMembers) { fail("room_full"); return; }
            }
            s.room = id; s.token.resize(16);
            for (int i = 0; i < 16; i += 4) {
                const auto random = QRandomGenerator::system()->generate();
                qToBigEndian(random, reinterpret_cast<uchar *>(s.token.data() + i));
            }
            result = QJsonObject{{"room", roomState(id)}, {"token", QString::fromLatin1(s.token.toHex())}};
            qInfo() << "session joined room" << s.id << id;
        } else { fail("unknown_operation"); return; }
        response["ok"] = true; response["data"] = result; send(s.socket, response);
        if (op == "join" || op == "create") changed(s.room);
    }
    void datagrams() {
        while (udp.hasPendingDatagrams()) {
            const auto datagram = udp.receiveDatagram(MaxDatagramBytes + 1);
            Packet p;
            if (!decodePacket(datagram.data(), p)) continue;
            Session *sender = nullptr;
            for (auto &s : sessions) if (s.id == p.sender && s.room && s.token == p.token) { sender = &s; break; }
            if (!sender || !allow(*sender, true) || datagram.senderAddress() != sender->socket->peerAddress()) continue;
            if (p.type == Register) {
                if (sender->udpPort && (sender->udpPort != datagram.senderPort() || sender->udpAddress != datagram.senderAddress())) continue;
                sender->udpAddress = datagram.senderAddress(); sender->udpPort = datagram.senderPort();
                p.type = Registered;
                udp.writeDatagram(encodePacket(p), sender->udpAddress, sender->udpPort);
            } else if (p.type == Audio && !sender->muted && sender->udpPort == datagram.senderPort()
                       && sender->udpAddress == datagram.senderAddress()) {
                for (const auto &recipient : sessions) {
                    if (recipient.id == sender->id || recipient.room != sender->room || !recipient.udpPort) continue;
                    p.token = recipient.token;
                    udp.writeDatagram(encodePacket(p), recipient.udpAddress, recipient.udpPort);
                }
            }
        }
    }
};

} // namespace vusiif::ztgui::voice_chat

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("ztgui-voice-server");
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOptions({{"listen", "Listen address", "address", "127.0.0.1"},
        {"tcp-port", "Control port", "port", QString::number(DefaultPort)},
        {"udp-port", "Audio port", "port", QString::number(DefaultPort)},
        {"max-rooms", "Room limit", "count", "32"},
        {"max-members", "Members per room", "count", "16"},
        {"max-connections", "Connection limit", "count", "256"}});
    parser.process(app);
    auto number = [&](const char *name, int limit) {
        bool ok = false; const int value = parser.value(name).toInt(&ok);
        if (!ok || value < 1 || value > limit) { qCritical() << "invalid option" << name; return -1; }
        return value;
    };
    const int tcpPort = number("tcp-port", 65535), udpPort = number("udp-port", 65535);
    const int roomLimit = number("max-rooms", 10000), memberLimit = number("max-members", 256), connectionLimit = number("max-connections", 10000);
    const QHostAddress address(parser.value("listen"));
    if (tcpPort < 0 || udpPort < 0 || roomLimit < 0 || memberLimit < 0 || connectionLimit < 0 || address.isNull()) return 2;
    Server server(roomLimit, memberLimit, connectionLimit);
    if (!server.listen(address, quint16(tcpPort), quint16(udpPort))) return 1;
    return app.exec();
}
