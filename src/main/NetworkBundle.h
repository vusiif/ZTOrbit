#pragma once
#include <QByteArray>
#include <QString>

namespace vusiif::ztgui::zerotier::bundle {
constexpr qint64 MaxFileSize = 8 * 1024 * 1024;
struct Configuration {
    QString networkId;
    QString moonFileName;
    QByteArray moonContents;
};
QByteArray encode(const Configuration &configuration);
bool decode(const QByteArray &bytes, Configuration &configuration, QString &error);
}
