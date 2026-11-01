#include "NetworkBundle.h"
#include "LocalConfig.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {
bool valid(const vusiif::zto::zerotier::bundle::Configuration &configuration)
{
    bool relayValid = configuration.tcpFallbackRelay.isEmpty();
    if (!relayValid) {
        QString ip;
        int port = 0;
        relayValid = vusiif::zto::zerotier::localconfig::parseDisplayRelay(
                         configuration.tcpFallbackRelay, ip, port)
            && port == vusiif::zto::zerotier::localconfig::RelayPort;
    }
    return relayValid
        && QRegularExpression("^[0-9a-fA-F]{16}$").match(configuration.networkId).hasMatch()
        && QRegularExpression("^[0-9a-fA-F]{16}\\.moon$", QRegularExpression::CaseInsensitiveOption)
            .match(configuration.moonFileName).hasMatch()
        && configuration.moonFileName.left(16).right(10).compare(configuration.networkId.left(10), Qt::CaseInsensitive) == 0
        && !configuration.moonContents.isEmpty() && configuration.moonContents.size() <= 4 * 1024 * 1024;
}
}

QByteArray vusiif::zto::zerotier::bundle::encode(const Configuration &configuration)
{
    if (!valid(configuration)) return {};
    QJsonObject object{
        {"format", "zto/network-moon"}, {"version", 1},
        {"networkId", configuration.networkId.toLower()},
        {"moon", QJsonObject{{"fileName", configuration.moonFileName},
            {"contentBase64", QString::fromLatin1(configuration.moonContents.toBase64())}}}
    };
    if (!configuration.tcpFallbackRelay.isEmpty()) {
        QString ip;
        int port = 0;
        if (!localconfig::parseDisplayRelay(configuration.tcpFallbackRelay, ip, port)
            || port != localconfig::RelayPort) return {};
        object["tcpFallbackRelay"] = localconfig::displayRelay(ip, localconfig::RelayPort);
    }
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

bool vusiif::zto::zerotier::bundle::decode(const QByteArray &bytes, Configuration &configuration, QString &error)
{
    configuration = {};
    if (bytes.size() > MaxFileSize) {
        error = QStringLiteral("配置包过大。");
        return false;
    }
    const auto document = QJsonDocument::fromJson(bytes);
    const auto object = document.object();
    if (!document.isObject() || object["format"].toString() != "zto/network-moon"
        || object["version"].toDouble() != 1) {
        error = QStringLiteral("不支持的中转站配置包。");
        return false;
    }
    QString tcpFallbackRelay;
    if (object.contains("tcpFallbackRelay")) {
        if (!object.value("tcpFallbackRelay").isString() || object.value("tcpFallbackRelay").toString().isEmpty()) {
            error = QStringLiteral("配置包的 tcpFallbackRelay 必须是非空字符串。");
            return false;
        }
        QString ip;
        int port = 0;
        if (!localconfig::parseDisplayRelay(object.value("tcpFallbackRelay").toString(), ip, port)
            || port != localconfig::RelayPort) {
            error = QStringLiteral("配置包的 tcpFallbackRelay 必须是有效的 IPv4:9443。");
            return false;
        }
        tcpFallbackRelay = localconfig::displayRelay(ip, localconfig::RelayPort);
    }
    const auto moon = object["moon"].toObject();
    const QString base64 = moon["contentBase64"].toString();
    const auto decoded = QByteArray::fromBase64Encoding(base64.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    Configuration candidate {object["networkId"].toString().toLower(), moon["fileName"].toString(), decoded.decoded,
                             tcpFallbackRelay};
    if (!decoded || !valid(candidate) || QString::fromLatin1(candidate.moonContents.toBase64()) != base64) {
        error = QStringLiteral("网络 ID、Moon 文件名或文件内容无效，或两者不匹配。");
        return false;
    }
    configuration = candidate;
    error.clear();
    return true;
}
