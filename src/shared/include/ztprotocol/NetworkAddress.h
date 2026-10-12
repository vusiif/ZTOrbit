#pragma once
#include <QHostAddress>
#include <QJsonObject>
#include <QJsonArray>

namespace vusiif::ztgui::protocol {
inline constexpr quint16 NetworkPort = 18100;
// ZTGUI networks use 255.255.255.0; the controller is always host .1.
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
