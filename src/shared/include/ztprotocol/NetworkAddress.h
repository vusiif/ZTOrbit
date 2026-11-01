#pragma once
#include <QHostAddress>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>

namespace vusiif::zto::protocol {
inline constexpr quint16 NetworkPort = 18100;
struct ServerEndpoint {
    QString host;
    quint16 port = NetworkPort;
    bool valid() const { return !host.isEmpty(); }
};
// Shared by client plugins: host, host:port, [IPv6]:port or bare IPv6.
// An omitted port always uses the common ZTOrbitHub port, never a saved port.
inline ServerEndpoint serverEndpoint(QString address) {
    address = address.trimmed();
    if (address.isEmpty() || address.contains('/') || address.contains('@')) return {};
    if (address.count(':') > 1 && !address.startsWith('[')) {
        if (QHostAddress(address).protocol() != QAbstractSocket::IPv6Protocol) return {};
        address = '[' + address + ']';
    }
    const QUrl url("tcp://" + address, QUrl::StrictMode);
    const int port = url.port(NetworkPort);
    if (!url.isValid() || url.host().isEmpty() || !url.path().isEmpty() || url.hasQuery() || url.hasFragment()
        || port < 1 || port > 65535 || address.endsWith(':')) return {};
    return {url.host(), quint16(port)};
}
// ZTOrbit networks use 255.255.255.0; the controller is always host .1.
inline QHostAddress networkServer(const QJsonObject &network) {
    for (const auto &value : network.value("assignedAddresses").toArray()) {
        QHostAddress address(value.toString().section('/', 0, 0));
        bool ipv4 = false;
        const auto ip = address.toIPv4Address(&ipv4);
        if (ipv4 && ip) return QHostAddress((ip & 0xffffff00u) | 1u);
    }
    return {};
}
}
