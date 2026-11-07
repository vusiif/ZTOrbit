#pragma once
#include <QString>
namespace vusiif::zto::update {
class UpdateConfig {
public:
    static constexpr int DefaultPort = 18102;
    static QString trustedPublicKeyBase64();
    static QString serverSettingKey();
    static QString serverAddress(const QString &settingsFile, QString &error);
    static bool setServer(const QString &settingsFile, const QString &ip, int port, QString &error);
};
}
