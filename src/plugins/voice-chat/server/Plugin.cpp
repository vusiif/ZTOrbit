#include <Protocol.h>
#include <ztserver/PluginInterface.h>
#include <QDateTime>
#include <QJsonArray>
#include <QMap>
#include <QRandomGenerator>
#include <QTimer>
#include <QDebug>
#include <limits>
#include <stdexcept>

using namespace vusiif::ztgui::voice_chat;
namespace vusiif::ztgui::voice_chat {
namespace api = vusiif::ztgui::server::api;
struct Session {
    api::ConnectionId connection = 0;
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

class VoiceServerPlugin final : public QObject, public api::PluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ZTSERVER_PLUGIN_IID FILE "plugin.json")
    Q_INTERFACES(vusiif::ztgui::server::api::PluginInterface)
public:
    api::PluginInfo info() const override { return {"voice-chat", QStringLiteral("语音聊天"), "0.1.0"}; }
    api::StartTask onStart(api::HostServices &host, const util::CancellationToken &) override {
        services = &host;
        const auto config = host.files().readFile("settings.json");
        if (config.ok()) {
            QJsonParseError error; const auto doc = QJsonDocument::fromJson(config.data, &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject()) throw std::runtime_error("Invalid voice settings.json");
            const auto settings = doc.object();
            maxRooms = settings.value("maxRooms").toInt(32);
            maxMembers = settings.value("maxMembers").toInt(16);
            maxConnections = settings.value("maxConnections").toInt(256);
            if (maxRooms < 1 || maxRooms > 10000 || maxMembers < 1 || maxMembers > 256 || maxConnections < 1 || maxConnections > 10000)
                throw std::runtime_error("Invalid voice capacity");
        }
        connect(&timer, &QTimer::timeout, this, [this] {
            const auto now = QDateTime::currentMSecsSinceEpoch();
            for (const auto connection : sessions.keys()) if (now - sessions[connection].lastSeen > 15000) services->disconnect(connection);
        });
        timer.start(1000);
        host.logs().write(util::LogLevel::Info, "Voice plugin started");
        return {};
    }
    void onStop() override {
        timer.stop(); sessions.clear(); rooms.clear();
        if (services) services->logs().write(util::LogLevel::Info,"Voice plugin stopped");
        services = nullptr;
    }
    void onConnectionClosed(api::ConnectionId connection) override {
        auto it = sessions.find(connection);
        if (it != sessions.end()) { leave(it.value()); sessions.erase(it); }
    }
    void onTcpPacket(api::ConnectionId connection, const QByteArray &payload) override {
        if (!sessions.contains(connection)) {
            if (sessions.size() >= maxConnections || nextSession == 0) { services->disconnect(connection); return; }
            Session session; session.connection = connection; session.id = nextSession++;
            session.lastSeen = session.rateWindow = QDateTime::currentMSecsSinceEpoch(); sessions.insert(connection, session);
        }
        auto &session = sessions[connection];
        if (payload.size() > MaxControlBytes || !allow(session, false)) { services->disconnect(connection); return; }
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(payload,&error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) { services->disconnect(connection); return; }
        session.lastSeen = QDateTime::currentMSecsSinceEpoch(); command(session,doc.object());
    }
    void onUdpPacket(const api::Endpoint &endpoint, const QByteArray &payload) override {
        datagram(endpoint,payload);
    }
private:
    api::HostServices *services = nullptr;
    QTimer timer;
    QMap<api::ConnectionId, Session> sessions;
    QMap<quint32, Room> rooms;
    quint32 nextSession = 1, nextRoom = 1;
    int maxRooms = 32, maxMembers = 16, maxConnections = 256;
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
    void send(api::ConnectionId connection, const QJsonObject &object) {
        const auto payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
        if (services->queuedTcpBytes(connection) + payload.size() > 256 * 1024) {
            // Queue closing to avoid erasing sessions during a room broadcast.
            QTimer::singleShot(0, this, [this, connection] { if (services) services->disconnect(connection); });
            return;
        }
        services->sendTcp(connection, payload);
    }
    void changed(quint32 room) {
        const auto list = roomList();
        for (const auto socket : sessions.keys()) {
            const auto s = sessions.value(socket);
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
            services->logs().write(util::LogLevel::Info, QString("Session %1 left room %2").arg(s.id).arg(oldRoom));
            changed(oldRoom);
        }
    }
    bool allow(Session &s, bool audio) {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (now - s.rateWindow >= 1000) { s.controls = s.packets = 0; s.rateWindow = now; }
        return audio ? ++s.packets <= 100 : ++s.controls <= 30;
    }
    void command(Session &s, const QJsonObject &request) {
        QJsonObject response{{"v", ProtocolVersion}, {"id", request.value("id")}, {"ok", false}};
        auto fail = [&](const QString &code) { response["error"] = code; send(s.connection, response); };
        if (request.value("v").toInt(-1) != ProtocolVersion) { fail("protocol_mismatch"); return; }
        const auto op = request.value("op").toString();
        const auto data = request.value("data").toObject();
        QJsonValue result;
        if (op == "hello") {
            if (s.hello) { fail("already_connected"); return; }
            const auto name = data.value("nickname").toString().trimmed();
            if (name.isEmpty() || name.size() > 64) { fail("invalid_nickname"); return; }
            s.nickname = name; s.hello = true;
            result = QJsonObject{{"session", double(s.id)}, {"udpPort", services->port()}, {"rooms", roomList()}};
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
            services->logs().write(util::LogLevel::Info, QString("Session %1 joined room %2").arg(s.id).arg(id));
        } else { fail("unknown_operation"); return; }
        response["ok"] = true; response["data"] = result; send(s.connection, response);
        if (op == "join" || op == "create") changed(s.room);
    }
    void datagram(const api::Endpoint &endpoint, const QByteArray &payload) {
            Packet p;
            if (!decodePacket(payload, p)) return;
            Session *sender = nullptr;
            for (auto &s : sessions) if (s.id == p.sender && s.room && s.token == p.token) { sender = &s; break; }
            if (!sender || !allow(*sender, true) || endpoint.address != services->peerAddress(sender->connection)) return;
            if (p.type == Register) {
                if (sender->udpPort && (sender->udpPort != endpoint.port || sender->udpAddress != endpoint.address)) return;
                sender->udpAddress = endpoint.address; sender->udpPort = endpoint.port;
                p.type = Registered;
                services->sendUdp({sender->udpAddress, sender->udpPort}, encodePacket(p));
            } else if (p.type == Audio && !sender->muted && sender->udpPort == endpoint.port
                       && sender->udpAddress == endpoint.address) {
                for (const auto &recipient : sessions) {
                    if (recipient.id == sender->id || recipient.room != sender->room || !recipient.udpPort) continue;
                    p.token = recipient.token;
                    services->sendUdp({recipient.udpAddress, recipient.udpPort}, encodePacket(p));
                }
            }
    }
};

} // namespace vusiif::ztgui::voice_chat

#include "Plugin.moc"
