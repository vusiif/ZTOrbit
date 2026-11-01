#include "VoiceConfig.h"

#include <QMap>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <algorithm>

namespace vusiif::zto::voice_chat {
namespace {
QSettings::Format legacyFormat()
{
#ifdef VOICE_TESTING
    // Tests redirect the INI user scope; never touch the real registry.
    return QSettings::IniFormat;
#else
    return QSettings::NativeFormat;
#endif
}

QString normalizedDevice(const QJsonValue &value)
{
    const auto text = value.toString().trimmed();
    static const QRegularExpression hex(QStringLiteral("\\A(?:[0-9a-fA-F]{2})+\\z"));
    return hex.match(text).hasMatch() ? text.toLower() : QString();
}

QStringList legacyKeys()
{
    return {QStringLiteral("address"), QStringLiteral("nickname"), QStringLiteral("volume"),
            QStringLiteral("inputVolume"), QStringLiteral("muted"), QStringLiteral("deafened"),
            QStringLiteral("input"), QStringLiteral("output")};
}

QJsonValue normalizedLegacyValue(const QString &key, const QVariant &value)
{
    if (key == QLatin1String("input") || key == QLatin1String("output"))
        return QString::fromLatin1(value.toByteArray().toHex());
    if (key == QLatin1String("volume") || key == QLatin1String("inputVolume"))
        return std::clamp(value.toInt(), 0, 200);
    if (key == QLatin1String("muted") || key == QLatin1String("deafened"))
        return value.toBool();
    return value.toString();
}

QJsonValue normalizedTargetValue(const QString &key, const QJsonValue &value)
{
    if (key == QLatin1String("input") || key == QLatin1String("output"))
        return normalizedDevice(value);
    if (key == QLatin1String("volume") || key == QLatin1String("inputVolume"))
        return std::clamp(value.toInt(), 0, 200);
    if (key == QLatin1String("muted") || key == QLatin1String("deafened"))
        return value.toBool();
    if (key == QLatin1String("nickname")) return value.toString().left(64);
    if (key == QLatin1String("address")) return value.toString().trimmed();
    return value.toString();
}

struct LegacyEntry {
    QJsonValue value;
    QString application;
};

// Every source key is tied to the application name it came from so only the
// keys actually used for the one-time import are cleared afterwards.
QMap<QString, LegacyEntry> legacySettings()
{
    QSettings current(legacyFormat(), QSettings::UserScope, QStringLiteral("ZTOrbit"),
                      QStringLiteral("ZTOrbit"));
    QSettings previous(legacyFormat(), QSettings::UserScope, QStringLiteral("ZTOrbit"),
                       QStringLiteral("ZTOrbit"));
    const QSettings *sources[]{&current, &previous};
    const QStringList applications{QStringLiteral("ZTOrbit"), QStringLiteral("ZTOrbit")};
    QMap<QString, LegacyEntry> result;
    for (const auto &key : legacyKeys()) {
        const auto path = QStringLiteral("voice-chat/") + key;
        for (int i = 0; i < 2; ++i) {
            if (!sources[i]->contains(path)) continue;
            result.insert(key, {normalizedLegacyValue(key, sources[i]->value(path)), applications[i]});
            break;
        }
    }
    return result;
}

bool parseObject(const QByteArray &data, QJsonObject &object, QString &error)
{
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(data, &parse);
    if (parse.error != QJsonParseError::NoError) { error = parse.errorString(); return false; }
    if (!document.isObject()) { error = QStringLiteral("不是 JSON 对象"); return false; }
    object = document.object();
    return true;
}

// Removes only source keys whose values were imported into the committed
// target. Conflicting, unknown and unimported keys stay untouched.
QStringList clearImportedLegacyKeys(const QMap<QString, LegacyEntry> &legacy,
                                    const QSet<QString> &used, const QJsonObject &committed)
{
    QStringList failures;
    for (auto it = legacy.cbegin(); it != legacy.cend(); ++it) {
        const auto key = it.key();
        const bool redundant = committed.contains(key)
            && normalizedTargetValue(key, committed.value(key)) == it->value;
        if (!used.contains(key) && !redundant) continue;
        const auto path = QStringLiteral("voice-chat/") + key;
        QSettings source(legacyFormat(), QSettings::UserScope, QStringLiteral("ZTOrbit"),
                         it->application);
        if (!source.contains(path)) continue;
        source.remove(path);
        source.sync();
        if (source.status() != QSettings::NoError) {
            failures.append(QStringLiteral("无法清除旧语音配置 %1：设置文件写入失败").arg(path));
            continue;
        }
        QSettings verify(legacyFormat(), QSettings::UserScope, QStringLiteral("ZTOrbit"),
                         it->application);
        if (verify.contains(path))
            failures.append(QStringLiteral("旧语音配置 %1 删除后仍存在").arg(path));
    }
    return failures;
}
} // namespace

VoiceConfigResult loadVoiceConfig(util::PluginStorage &storage)
{
    VoiceConfigResult result;
    const auto importState = storage.legacyImportAllowed();
    const bool canImport = importState.ok() && importState.allowed;
    if (!importState.ok()) {
        result.error = QStringLiteral("无法读取旧配置导入状态：%1").arg(importState.error);
    }
    const auto legacy = canImport ? legacySettings() : QMap<QString, LegacyEntry>();
    QSet<QString> usedLegacy;
    const auto transaction = storage.updateFile(QStringLiteral("config.json"),
        [&](const util::PluginFileSnapshot &snapshot) {
            QJsonObject object;
            if (snapshot.exists && !snapshot.data.isEmpty()) {
                QString parseError;
                if (!parseObject(snapshot.data, object, parseError))
                    return util::PluginFileUpdate::failure(
                        QStringLiteral("语音配置损坏（%1），已保留原文件，请修复或删除后重新加载插件。").arg(parseError));
            }
            bool changed = !snapshot.exists;
            const auto ensure = [&](const QString &key, const QJsonValue &fallback) -> QJsonValue {
                if (object.contains(key)) return object.value(key);
                changed = true;
                const auto legacyValue = legacy.value(key);
                if (!legacyValue.application.isEmpty()) {
                    usedLegacy.insert(key);
                    object[key] = legacyValue.value;
                } else {
                    object[key] = fallback;
                }
                return object.value(key);
            };
            const auto normalize = [&](const QString &key, const QJsonValue &value) {
                if (object.value(key) == value) return;
                object[key] = value;
                changed = true;
            };
            normalize(QStringLiteral("format"), 1);
            const auto address = ensure(QStringLiteral("address"), QString()).toString();
            normalize(QStringLiteral("address"), address.trimmed());
            const auto nickname = ensure(QStringLiteral("nickname"), QString()).toString();
            normalize(QStringLiteral("nickname"), nickname.left(64));
            const int volume = std::clamp(ensure(QStringLiteral("volume"), 80).toInt(), 0, 200);
            normalize(QStringLiteral("volume"), volume);
            const int inputVolume = std::clamp(ensure(QStringLiteral("inputVolume"), 100).toInt(), 0, 200);
            normalize(QStringLiteral("inputVolume"), inputVolume);
            normalize(QStringLiteral("muted"), ensure(QStringLiteral("muted"), false).toBool());
            normalize(QStringLiteral("deafened"), ensure(QStringLiteral("deafened"), false).toBool());
            normalize(QStringLiteral("input"), normalizedDevice(ensure(QStringLiteral("input"), QString())));
            normalize(QStringLiteral("output"), normalizedDevice(ensure(QStringLiteral("output"), QString())));
            if (!changed) return util::PluginFileUpdate::keep();
            return util::PluginFileUpdate::replace(QJsonDocument(object).toJson(QJsonDocument::Indented));
        });
    if (!transaction.ok) {
        result.error = transaction.error;
        result.config.writable = false;
        return result;
    }
    QJsonObject object;
    QString parseError;
    if (!parseObject(transaction.data, object, parseError)) {
        result.error = QStringLiteral("语音配置读取校验失败：%1").arg(parseError);
        result.config.writable = false;
        return result;
    }
    result.config.address = object.value("address").toString();
    result.config.nickname = object.value("nickname").toString();
    result.config.volume = std::clamp(object.value("volume").toInt(80), 0, 200);
    result.config.inputVolume = std::clamp(object.value("inputVolume").toInt(100), 0, 200);
    result.config.muted = object.value("muted").toBool();
    result.config.deafened = object.value("deafened").toBool();
    result.config.input = QByteArray::fromHex(object.value("input").toString().toLatin1());
    result.config.output = QByteArray::fromHex(object.value("output").toString().toLatin1());
    if (canImport) {
        // Only the target commit above may authorize source removal. A failed
        // source cleanup leaves the import open so the next load retries it;
        // silently marking the import as completed would strand old keys.
        const auto cleanupFailures = clearImportedLegacyKeys(legacy, usedLegacy, object);
        if (!cleanupFailures.isEmpty()) {
            result.warnings.append(cleanupFailures);
            return result;
        }
        // Commit the one-way marker only after the target was written, read
        // back and its imported source keys were removed.
        const auto completed = storage.completeLegacyImport();
        if (!completed.ok() && result.error.isEmpty())
            result.error = QStringLiteral("语音配置已保存，但无法关闭旧配置导入：%1").arg(completed.error);
    }
    return result;
}

bool saveVoiceConfigField(util::PluginStorage &storage, const QString &key, const QJsonValue &value,
                          QString &error)
{
    const auto transaction = storage.updateFile(QStringLiteral("config.json"),
        [&](const util::PluginFileSnapshot &snapshot) {
            if (!snapshot.exists || snapshot.data.isEmpty())
                return util::PluginFileUpdate::failure(QStringLiteral("语音配置不存在，无法只保存本次修改"));
            QJsonObject object;
            QString parseError;
            if (!parseObject(snapshot.data, object, parseError))
                return util::PluginFileUpdate::failure(
                    QStringLiteral("语音配置损坏（%1），已保留原文件").arg(parseError));
            if (object.value(key) == value) return util::PluginFileUpdate::keep();
            object[key] = value;
            object[QStringLiteral("format")] = 1;
            return util::PluginFileUpdate::replace(QJsonDocument(object).toJson(QJsonDocument::Indented));
        });
    error = transaction.error;
    return transaction.ok;
}

bool updateMemberRemark(util::PluginStorage &storage, const QString &ztid, const QString &remark,
                        QJsonObject &resultObject, QString &error)
{
    const auto transaction = storage.updateFile(QStringLiteral("member-remarks.json"),
        [&](const util::PluginFileSnapshot &snapshot) {
            QJsonObject object;
            if (snapshot.exists) {
                if (snapshot.data.isEmpty())
                    return util::PluginFileUpdate::failure(QStringLiteral("成员备注文件损坏（文件为空），已保留原文件"));
                QString parseError;
                if (!parseObject(snapshot.data, object, parseError))
                    return util::PluginFileUpdate::failure(
                        QStringLiteral("成员备注文件损坏（%1），已保留原文件").arg(parseError));
            }
            const auto name = remark.trimmed().left(64);
            if (name.isEmpty()) {
                if (!object.contains(ztid)) {
                    resultObject = object;
                    return util::PluginFileUpdate::keep();
                }
                object.remove(ztid);
            } else {
                if (object.value(ztid).toString() == name) {
                    resultObject = object;
                    return util::PluginFileUpdate::keep();
                }
                object[ztid] = name;
            }
            resultObject = object;
            return util::PluginFileUpdate::replace(QJsonDocument(object).toJson(QJsonDocument::Indented));
        });
    if (!transaction.ok) {
        error = transaction.error;
        return false;
    }
    QString parseError;
    QJsonObject finalObject;
    if (!parseObject(transaction.data, finalObject, parseError)) {
        error = QStringLiteral("成员备注写入后读取校验失败：%1").arg(parseError);
        return false;
    }
    resultObject = finalObject;
    return true;
}

} // namespace vusiif::zto::voice_chat
