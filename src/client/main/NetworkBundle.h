#pragma once
#include <QByteArray>
#include <QString>

namespace vusiif::zto::zerotier::bundle {
constexpr qint64 MaxFileSize = 8 * 1024 * 1024;
struct Configuration {
    QString networkId;
    QString moonFileName;
    QByteArray moonContents;
    // Optional version=1 field. UI and bundle use IP:9443; local.conf uses IP/9443.
    QString tcpFallbackRelay;
};
QByteArray encode(const Configuration &configuration);
bool decode(const QByteArray &bytes, Configuration &configuration, QString &error);
}
