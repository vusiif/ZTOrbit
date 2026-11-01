#pragma once
#include <ztprotocol/NetworkAddress.h>
#include <ztprotocol/WireProtocol.h>
#include <QNetworkProxy>
#include <QUdpSocket>
#include <QSet>
#include <QRegularExpression>
#include <functional>

namespace vusiif::zto::util {
struct SendResult {
    int recipients = 0;
    QString error;
    QStringList failedMembers;
    // Success means queued to the local UDP socket, not remote delivery.
    bool ok() const { return error.isEmpty(); }
};
using SendCallback = std::function<void(const SendResult &)>;
// Send ZTS1 datagrams to authorized members on 18100 using the calling plugin route.
inline SendResult sendNetworkData(const QJsonArray &members, const QString &memberId,
                                 const QByteArray &route, const QByteArray &data) {
    const auto packet = protocol::encode(route, data);
    if (packet.isEmpty() || packet.size() > protocol::MaxUdpBytes)
        return {0, QStringLiteral("数据超出 UDP 路由包上限或插件 ID 无效")};
    if (!memberId.isEmpty() && !QRegularExpression("^[0-9a-fA-F]{10}$").match(memberId).hasMatch())
        return {0, QStringLiteral("成员 ID 无效")};
    QSet<QHostAddress> targets;
    for (const auto &value : members) {
        const auto member = value.toObject();
        if (!member.value("authorized").toBool() || (!memberId.isEmpty()
            && member.value("nodeId").toString().compare(memberId, Qt::CaseInsensitive))) continue;
        for (const auto &ip : member.value("ipAssignments").toArray()) {
            const QHostAddress address(ip.toString().section('/', 0, 0));
            if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isNull()
                && !address.isLoopback() && !address.isMulticast()
                && (address.toIPv4Address() & 255u) != 0 && (address.toIPv4Address() & 255u) != 255) {
                targets.insert(address); break;
            }
        }
    }
    if (targets.isEmpty()) return {0, QStringLiteral("没有可发送的已授权成员 IPv4 地址")};
    QUdpSocket socket;
    socket.setProxy(QNetworkProxy::NoProxy);
    SendResult result;
    for (const auto &address : targets) {
        if (socket.writeDatagram(packet, address, protocol::NetworkPort) != packet.size()) {
            result.error = socket.errorString(); return result;
        }
        ++result.recipients;
    }
    return result;
}
}
