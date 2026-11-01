#pragma once
#include <ztprotocol/WireProtocol.h>
#include <ztprotocol/PluginIdentity.h>
#include <QJsonArray>
#include <QJsonObject>
#include <QHostAddress>
#include <QRegularExpression>

namespace vusiif::zto::protocol {
inline constexpr quint16 ClientNetworkPort = 18101;
inline bool validNetworkId(const QString &id) {
    static const QRegularExpression re("\\A[0-9a-fA-F]{16}\\z"); return re.match(id).hasMatch();
}
inline bool validMemberId(const QString &id) {
    static const QRegularExpression re("\\A[0-9a-fA-F]{10}\\z"); return re.match(id).hasMatch();
}
// ZTS1/builtin + ZTD1 + network(8), sender(5), target(5), message(16), route length(1).
inline int maxNetworkDataBytes(const QString &pluginId) { return MaxUdpBytes - 52 - pluginId.toLatin1().size(); }
struct Delivery { QString network, sender, target, plugin; QByteArray messageId, data; };
inline QByteArray encodeDelivery(const Delivery &d) {
    if (!validNetworkId(d.network) || !validMemberId(d.sender) || !validMemberId(d.target)
        || !validPluginId(d.plugin) || d.messageId.size() != 16 || d.data.size() > maxNetworkDataBytes(d.plugin)) return {};
    QByteArray p("ZTD1", 4);
    p += QByteArray::fromHex(d.network.toLatin1()); p += QByteArray::fromHex(d.sender.toLatin1());
    p += QByteArray::fromHex(d.target.toLatin1()); p += d.messageId;
    p += char(d.plugin.size()); p += d.plugin.toLatin1(); p += d.data;
    return encode("builtin", p);
}
inline bool decodeDelivery(const QByteArray &p, Delivery &d) {
    if (p.size() < 40 || p.left(4) != "ZTD1") return false;
    const int size = quint8(p[38]);
    if (size < 1 || size > 128 || p.size() < 39 + size) return false;
    d = {QString::fromLatin1(p.mid(4, 8).toHex()), QString::fromLatin1(p.mid(12, 5).toHex()),
         QString::fromLatin1(p.mid(17, 5).toHex()), QString::fromLatin1(p.mid(39, size)), p.mid(22, 16), p.mid(39 + size)};
    return validPluginId(d.plugin) && p.size() + 13 <= MaxUdpBytes;
}
inline QHostAddress memberAddress(const QJsonObject &member) {
    if (!member.value("authorized").toBool()) return {};
    for (const auto &ip : member.value("ipAssignments").toArray()) {
        QHostAddress address(ip.toString().section('/', 0, 0));
        const auto v = address.toIPv4Address();
        if (address.protocol() == QAbstractSocket::IPv4Protocol
            && !address.isMulticast() && (v & 255) != 0 && (v & 255) != 255) return address;
    }
    return {};
}
}
