#include "UpdateConfig.h"
#if defined(__has_include)
#  if __has_include("UpdateDefaults.h")
#    include "UpdateDefaults.h"
#  endif
#endif
#ifndef ZTO_UPDATE_PUBLIC_KEY_BASE64
#define ZTO_UPDATE_PUBLIC_KEY_BASE64 ""
#endif
#include <QHostAddress>
#include <QSettings>
#include <QUrl>
namespace vusiif::zto::update {
namespace {
QString address(const QString &ip, int port, QString &error)
{
    QHostAddress host;
    if (!host.setAddress(ip.trimmed()) || host.protocol() != QAbstractSocket::IPv4Protocol) {
        error = QStringLiteral("请输入有效的 IPv4 地址");
        return {};
    }
    if (port < 1 || port > 65535) {
        error = QStringLiteral("端口应为 1–65535");
        return {};
    }
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(host.toString());
    url.setPort(port);
    return url.toString();
}
}
QString UpdateConfig::trustedPublicKeyBase64() { return QString::fromUtf8(ZTO_UPDATE_PUBLIC_KEY_BASE64).trimmed(); }
QString UpdateConfig::serverSettingKey() { return QStringLiteral("update/server"); }
QString UpdateConfig::serverAddress(const QString &settingsFile, QString &error)
{
    error.clear();
    if (settingsFile.isEmpty()) return {};
    QSettings settings(settingsFile, QSettings::IniFormat);
    const auto saved = settings.value(serverSettingKey()).toString().trimmed();
    if (settings.status() != QSettings::NoError) { error = QStringLiteral("无法读取更新服务器设置"); return {}; }
    if (saved.isEmpty()) return {};
    const QUrl url(saved, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("http") || !url.userInfo().isEmpty()
        || url.hasQuery() || url.hasFragment() || !url.path().isEmpty()) {
        error = QStringLiteral("更新服务器设置无效"); return {};
    }
    return address(url.host(), url.port(DefaultPort), error);
}
bool UpdateConfig::setServer(const QString &settingsFile, const QString &ip, int port, QString &error)
{
    error.clear();
    if (settingsFile.isEmpty()) { error = QStringLiteral("设置文件路径为空"); return false; }
    const auto value = ip.trimmed().isEmpty() ? QString() : address(ip, port, error);
    if (!error.isEmpty()) return false;
    QSettings settings(settingsFile, QSettings::IniFormat);
    if (value.isEmpty()) settings.remove(serverSettingKey());
    else settings.setValue(serverSettingKey(), value);
    settings.sync();
    if (settings.status() != QSettings::NoError) { error = QStringLiteral("无法保存更新服务器"); return false; }
    return true;
}
}
