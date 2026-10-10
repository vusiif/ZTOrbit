#include "NetworkBundle.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {
bool valid(const NetworkBundle::Configuration &configuration)
{
    return QRegularExpression("^[0-9a-fA-F]{16}$").match(configuration.networkId).hasMatch()
        && QRegularExpression("^[0-9a-fA-F]{16}\\.moon$", QRegularExpression::CaseInsensitiveOption)
            .match(configuration.moonFileName).hasMatch()
        && configuration.moonFileName.left(16).right(10).compare(configuration.networkId.left(10), Qt::CaseInsensitive) == 0
        && !configuration.moonContents.isEmpty() && configuration.moonContents.size() <= 4 * 1024 * 1024;
}
}

QByteArray NetworkBundle::encode(const Configuration &configuration)
{
    if (!valid(configuration)) return {};
    return QJsonDocument(QJsonObject{
        {"format", "zerotier-gui/network-moon"}, {"version", 1},
        {"networkId", configuration.networkId.toLower()},
        {"moon", QJsonObject{{"fileName", configuration.moonFileName},
            {"contentBase64", QString::fromLatin1(configuration.moonContents.toBase64())}}}
    }).toJson(QJsonDocument::Indented);
}

bool NetworkBundle::decode(const QByteArray &bytes, Configuration &configuration, QString &error)
{
    configuration = {};
    if (bytes.size() > MaxFileSize) {
        error = QStringLiteral("配置包过大。");
        return false;
    }
    const auto document = QJsonDocument::fromJson(bytes);
    const auto object = document.object();
    if (!document.isObject() || object["format"].toString() != "zerotier-gui/network-moon"
        || object["version"].toDouble() != 1) {
        error = QStringLiteral("不支持的中转站配置包。");
        return false;
    }
    const auto moon = object["moon"].toObject();
    const QString base64 = moon["contentBase64"].toString();
    const auto decoded = QByteArray::fromBase64Encoding(base64.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    Configuration candidate {object["networkId"].toString().toLower(), moon["fileName"].toString(), decoded.decoded};
    if (!decoded || !valid(candidate) || QString::fromLatin1(candidate.moonContents.toBase64()) != base64) {
        error = QStringLiteral("网络 ID、Moon 文件名或文件内容无效，或两者不匹配。");
        return false;
    }
    configuration = candidate;
    error.clear();
    return true;
}
