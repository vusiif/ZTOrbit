#include "NetworkMembers.h"
#include <ztprotocol/NetworkAddress.h>
#include <ztprotocol/WireProtocol.h>
#include <QTcpSocket>
#include <QNetworkProxy>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <memory>

namespace vusiif::ztgui::zerotier {
void requestMembers(QObject *context, const QHostAddress &server, const QString &networkId,
                    util::JsonCallback completed) {
    auto *job = new QObject(context);
    auto *socket = new QTcpSocket(job);
    socket->setProxy(QNetworkProxy::NoProxy);
    socket->setReadBufferSize(protocol::MaxTcpBytes + 4);
    auto *timeout = new QTimer(job);
    timeout->setSingleShot(true);
    auto finished = std::make_shared<bool>(false);
    auto input = std::make_shared<QByteArray>();
    const auto done = [job, socket, timeout, finished, completed](util::JsonResult result) {
        if (*finished) return;
        *finished = true;
        timeout->stop();
        socket->abort();
        job->deleteLater();
        if (completed) completed(result);
    };
    QObject::connect(timeout, &QTimer::timeout, job, [done] { done({{}, QStringLiteral("成员列表请求超时")}); });
    QObject::connect(socket, &QTcpSocket::errorOccurred, job, [done, socket] { done({{}, socket->errorString()}); });
    QObject::connect(socket, &QTcpSocket::disconnected, job, [done] { done({{}, QStringLiteral("服务器已断开连接")}); });
    QObject::connect(socket, &QTcpSocket::connected, job, [socket, networkId] {
        const QJsonObject request{{"v", 1}, {"id", 1}, {"op", "controller.members"},
                                  {"data", QJsonObject{{"networkId", networkId.toLower()}}}};
        socket->write(protocol::frame("builtin", QJsonDocument(request).toJson(QJsonDocument::Compact)));
    });
    QObject::connect(socket, &QTcpSocket::readyRead, job, [done, socket, input] {
        *input += socket->readAll();
        protocol::Packet packet;
        const auto status = protocol::takeFrame(*input, packet);
        if (status == protocol::ReadResult::Incomplete) return;
        const auto object = QJsonDocument::fromJson(packet.payload).object();
        if (status != protocol::ReadResult::Packet || packet.plugin != "builtin"
            || object.value("v").toInt() != 1 || object.value("id").toInt() != 1) {
            done({{}, QStringLiteral("成员列表响应无效")}); return;
        }
        if (!object.value("ok").toBool()) {
            done({{}, object.value("error").toString(QStringLiteral("成员列表查询失败"))}); return;
        }
        if (!object.value("data").isArray()) { done({{}, QStringLiteral("成员列表格式无效")}); return; }
        done({QJsonDocument(object.value("data").toArray()), {}});
    });
    QTimer::singleShot(0, job, [server, networkId, socket, timeout, done] {
        if (server.isNull() || !QRegularExpression("^[0-9a-fA-F]{16}$").match(networkId).hasMatch()) {
            done({{}, QStringLiteral("网络缺少 IPv4 地址或网络 ID 无效")}); return;
        }
        timeout->start(20000);
        socket->connectToHost(server, protocol::NetworkPort);
    });
}
}
