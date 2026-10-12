#include "ServerHost.h"
#include "PluginServices.h"
#include "AppLog.h"
#include <ztprotocol/WireProtocol.h>
#include <QDir>
#include <QNetworkDatagram>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDateTime>
#include <QNetworkProxy>
#include <QJsonArray>
#include <QThread>
#include <stdexcept>
#include <QDebug>
#include <limits>
namespace vusiif::ztgui::server {
namespace wire = protocol;
ServerHost::ServerHost(QString directory) : m_directory(std::move(directory)), m_queries(this) {
    m_udp.setProxy(QNetworkProxy::NoProxy);
    connect(&m_tcp, &QTcpServer::newConnection, this, [this] { accept(); });
    connect(&m_udp, &QUdpSocket::readyRead, this, [this] { datagrams(); });
    connect(&m_timer, &QTimer::timeout, this, [this] { poll(); });
}
ServerHost::~ServerHost() { stop(); }
bool ServerHost::start(const QHostAddress &address, quint16 port) {
    if (!m_tcp.listen(address, port)) { qCritical() << "TCP bind failed:" << m_tcp.errorString(); return false; }
    if (!m_udp.bind(address, m_tcp.serverPort())) { qCritical() << "UDP bind failed:" << m_udp.errorString(); m_tcp.close(); return false; }
    scan(); m_timer.start(100);
    qInfo() << "zt-server listening" << address << "TCP/UDP" << m_tcp.serverPort(); return true;
}
void ServerHost::scan() {
    const QDir directory(QDir(m_directory).filePath("plugins"));
    for (const auto &file : directory.entryInfoList({"*.so"}, QDir::Files | QDir::NoSymLinks, QDir::Name)) {
        Plugin loaded; loaded.library = std::make_unique<QPluginLoader>(file.absoluteFilePath());
        loaded.library->setLoadHints(QLibrary::ResolveAllSymbolsHint);
        const auto metadata = loaded.library->metaData();
        if (metadata.value("IID").toString() != QLatin1String(ZTSERVER_PLUGIN_IID) ||
            metadata.value("MetaData").toObject().value("apiVersion").toString() != QLatin1String(api::ApiVersion)) {
            qWarning() << "Rejected plugin metadata" << file.fileName(); continue;
        }
        try {
            loaded.entry = qobject_cast<api::PluginInterface *>(loaded.library->instance());
            if (!loaded.entry) throw std::runtime_error("plugin entry unavailable");
            loaded.info = loaded.entry->info();
            const auto &info = loaded.info;
            if (!wire::validId(info.id.toLatin1()) || info.id.toLatin1() != info.id.toUtf8() || info.id == "builtin" ||
                info.apiMajor != api::ApiMajor || info.apiMinor != api::ApiMinor || info.name.trimmed().isEmpty() ||
                plugin(info.id)) throw std::runtime_error("invalid, duplicate or incompatible plugin");
            loaded.services = std::make_unique<PluginServices>(*this, info.id, info.name, m_directory);
            auto task = loaded.entry->onStart(*loaded.services, loaded.cancellation.token());
            if (task) loaded.task = std::async(std::launch::async, [task = std::move(task)] {
                try { return task(); }
                catch (const std::exception &) { return api::StartResult{false, "Plugin startup exception"}; }
                catch (...) { return api::StartResult{false, "Plugin startup exception"}; }
            });
            else loaded.ready = true;
            qInfo() << "Loaded plugin" << info.id;
            m_plugins.push_back(std::move(loaded));
        } catch (const std::exception &error) {
            qWarning() << "Plugin rejected" << file.fileName() << error.what();
            if (loaded.services) { loaded.services->stopRequests(); try { loaded.entry->onStop(); } catch (...) {} }
            loaded.library->unload();
        } catch (...) {
            qWarning() << "Plugin rejected" << file.fileName();
            if (loaded.services) { loaded.services->stopRequests(); try { loaded.entry->onStop(); } catch (...) {} }
            loaded.library->unload();
        }
    }
}
ServerHost::Plugin *ServerHost::plugin(const QString &id) {
    for (auto &entry : m_plugins) if (entry.info.id == id) return &entry;
    return nullptr;
}
void ServerHost::poll() {
    for (auto &entry : m_plugins) if (entry.task.valid() && entry.task.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto result = entry.task.get(); entry.ready = result.success;
        if (!result.success) qWarning() << "Plugin initialization failed" << entry.info.id << result.message;
    }
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto id : m_connections.keys()) if (now - m_connections.value(id).lastSeen > 30000) disconnect(id);
}
void ServerHost::accept() {
    while (m_tcp.hasPendingConnections()) {
        auto *socket = m_tcp.nextPendingConnection();
        if (m_stopping || m_connections.size() >= 256 || m_next == 0) { socket->abort(); socket->deleteLater(); continue; }
        socket->setReadBufferSize(wire::MaxTcpBytes + 4);
        const auto id = m_next++;
        Connection state; state.socket = socket; state.window = state.lastSeen = QDateTime::currentMSecsSinceEpoch();
        m_connections.insert(id, state);
        connect(socket, &QTcpSocket::readyRead, this, [this, id] { read(id); });
        connect(socket, &QTcpSocket::disconnected, this, [this, id, socket] {
            const auto routes = m_connections.take(id).routes;
            for (const auto &route : routes) if (auto *entry = plugin(route)) {
                try { entry->entry->onConnectionClosed(id); }
                catch (...) { qWarning() << "Plugin connection callback failed" << route; }
            }
            socket->deleteLater();
        });
    }
}
void ServerHost::read(api::ConnectionId id) {
    auto it = m_connections.find(id);
    if (it == m_connections.end() || m_stopping) return;
    it->input += it->socket->readAll();
    for (;;) {
        it = m_connections.find(id);
        if (it == m_connections.end()) return;
        wire::Packet packet;
        const auto result = wire::takeFrame(it->input, packet);
        if (result == wire::ReadResult::Incomplete) return;
        if (result == wire::ReadResult::Invalid) { disconnect(id); return; }
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (now - it->window >= 1000) { it->window = now; it->requests = 0; }
        if (++it->requests > 60) { disconnect(id); return; }
        it->lastSeen = now;
        const QString route = QString::fromLatin1(packet.plugin);
        if (route == "builtin") { builtin(id, packet.payload); continue; }
        auto *entry = plugin(route);
        if (!entry || !entry->ready) {
            sendTcp("builtin", id, QJsonDocument(QJsonObject{{"v",1},{"ok",false},{"error","unknown_or_unready_plugin"},{"plugin",route}}).toJson(QJsonDocument::Compact));
            continue;
        }
        it->routes.insert(route);
        try { entry->entry->onTcpPacket(id, packet.payload); }
        catch (...) { qWarning() << "Plugin packet callback failed" << route; disconnect(id); return; }
    }
}
void ServerHost::datagrams() {
    while (m_udp.hasPendingDatagrams()) {
        const auto datagram = m_udp.receiveDatagram(wire::MaxUdpBytes + 1);
        if (m_stopping || datagram.data().size() > wire::MaxUdpBytes) continue;
        wire::Packet packet;
        if (!wire::decode(datagram.data(), packet)) continue;
        auto *entry = plugin(QString::fromLatin1(packet.plugin));
        if (!entry || !entry->ready) continue;
        try { entry->entry->onUdpPacket({datagram.senderAddress(), quint16(datagram.senderPort())}, packet.payload); }
        catch (...) { qWarning() << "Plugin UDP callback failed" << entry->info.id; }
    }
}
void ServerHost::builtin(api::ConnectionId id, const QByteArray &payload) {
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(payload, &parse);
    const auto request = document.object();
    if (parse.error != QJsonParseError::NoError || !document.isObject() || request.value("v").toInt() != 1 || !request.contains("id")) { disconnect(id); return; }
    const auto it = m_connections.find(id);
    if (it == m_connections.end()) return;
    m_queries.query(request.value("op").toString(), request.value("data").toObject(), it->socket,
        [this, id, serial = request.value("id")](const api::JsonResult &result) {
            QJsonObject response{{"v",1},{"id",serial},{"ok",result.ok()}};
            if (result.ok()) response["data"] = result.document.isArray() ? QJsonValue(result.document.array()) : QJsonValue(result.document.object());
            else response["error"] = result.error;
            sendTcp("builtin", id, QJsonDocument(response).toJson(QJsonDocument::Compact));
        });
}
QHostAddress ServerHost::peerAddress(api::ConnectionId id) const {
    const auto it = m_connections.constFind(id); return it == m_connections.cend() ? QHostAddress() : it->socket->peerAddress();
}
bool ServerHost::sendTcp(const QString &route, api::ConnectionId id, const QByteArray &payload) {
    if (m_stopping || QThread::currentThread() != thread()) return false;
    const auto it = m_connections.find(id);
    if (it == m_connections.end()) return false;
    const auto packet = wire::frame(route.toLatin1(), payload);
    if (packet.isEmpty()) return false;
    if (it->socket->bytesToWrite() + packet.size() > 2 * wire::MaxTcpBytes) {
        QTimer::singleShot(0, this, [this,id] { disconnect(id); }); return false;
    }
    return it->socket->write(packet) == packet.size();
}
bool ServerHost::sendUdp(const QString &route, const api::Endpoint &endpoint, const QByteArray &payload) {
    if (m_stopping || QThread::currentThread() != thread() || endpoint.address.isNull() || !endpoint.port) return false;
    const auto packet = wire::encode(route.toLatin1(), payload);
    if (packet.isEmpty() || packet.size() > wire::MaxUdpBytes) return false;
    return m_udp.writeDatagram(packet, endpoint.address, endpoint.port) == packet.size();
}
void ServerHost::disconnect(api::ConnectionId id) {
    if (QThread::currentThread() != thread()) return;
    const auto it = m_connections.find(id); if (it != m_connections.end()) it->socket->abort();
}
qint64 ServerHost::queuedTcpBytes(api::ConnectionId id) const {
    if (QThread::currentThread() != thread()) return -1;
    const auto it = m_connections.constFind(id);
    return it == m_connections.cend() ? -1 : it->socket->bytesToWrite();
}
void ServerHost::stop() {
    if (m_stopping) return;
    m_stopping = true; m_timer.stop(); m_tcp.close(); m_udp.close(); m_queries.stop();
    for (auto &entry : m_plugins) { entry.ready = false; entry.cancellation.requestCancellation(); entry.services->stopRequests(); }
    for (const auto id : m_connections.keys()) disconnect(id);
    for (auto &entry : m_plugins) {
        // Consume the future so plugin-owned task captures die before unloading.
        if (entry.task.valid()) entry.task.get();
        try { entry.entry->onStop(); } catch (...) { qWarning() << "Plugin stop failed" << entry.info.id; }
        entry.library->unload(); entry.services.reset();
    }
    m_plugins.clear();
}
}
