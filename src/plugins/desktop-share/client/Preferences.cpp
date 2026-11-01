#include "Preferences.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <zto/HostServices.h>
#include <zto/PluginStorage.h>

namespace vusiif::zto::desktop_share {

namespace {
constexpr const char *AddressKey = "serverAddress";
constexpr const char *ProfileKey = "profile";

QString validProfile(const QString &profile)
{
    if (profile == QStringLiteral("720p30") || profile == QStringLiteral("1080p30"))
        return profile;
    return {};
}
} // namespace

Preferences::Preferences(util::HostServices *services) : m_services(services) {}

QString Preferences::relativePath()
{
    return QStringLiteral("preferences.json");
}

Preferences::Values Preferences::load()
{
    m_error.clear();
    Values values;
    if (!m_services) {
        m_error = QStringLiteral("宿主文件服务不可用");
        values.error = m_error;
        return values;
    }
    QByteArray data;
    bool exists = false;
    auto *files = &m_services->files();
    if (auto *storage = dynamic_cast<util::PluginStorage *>(files)) {
        // The snapshot is taken under the host's cross-process coordination lock,
        // so a concurrent field merge by another process is never read halfway.
        const auto transaction = storage->updateFile(
            relativePath(), [](const util::PluginFileSnapshot &) {
                return util::PluginFileUpdate::keep();
            });
        if (!transaction.ok) {
            m_error = transaction.error;
            values.error = m_error;
            return values;
        }
        data = transaction.data;
        exists = !data.isNull();
    } else {
        const auto read = files->readFile(relativePath());
        if (!read.ok()) {
            // This host cannot report file existence through a transaction, so a
            // read failure is treated as "no preference yet". Saving still goes
            // through the mandatory field-merging path and reports visibly if
            // that extension is missing.
            values.loaded = true;
            return values;
        }
        data = read.data;
        exists = !data.isNull();
    }
    if (!exists || data.trimmed().isEmpty()) {
        values.loaded = true;
        return values;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        m_error = QStringLiteral("偏好文件无法解析：%1；未使用旧值也未覆盖文件")
                      .arg(parseError.errorString());
        values.error = m_error;
        return values;
    }
    const auto object = document.object();
    values.serverAddress = object.value(QLatin1String(AddressKey)).toString().trimmed();
    values.profile = validProfile(object.value(QLatin1String(ProfileKey)).toString());
    values.loaded = true;
    return values;
}

bool Preferences::save(const Change &change, Values *merged, QString *error)
{
    m_error.clear();
    const auto fail = [this, error](const QString &message) {
        m_error = message;
        if (error) *error = message;
        return false;
    };
    if (!m_services) return fail(QStringLiteral("宿主文件服务不可用"));
    if (!change.serverAddressDirty && !change.profileDirty) return true;
    const auto address = change.serverAddress.trimmed();
    const auto profile = validProfile(change.profile);
    if (change.profileDirty && profile.isEmpty() && !change.profile.isEmpty())
        return fail(QStringLiteral("档位无效，未保存偏好：%1").arg(change.profile));
    auto *files = &m_services->files();
    auto *storage = dynamic_cast<util::PluginStorage *>(files);
    if (!storage)
        return fail(QStringLiteral("宿主文件服务不支持跨进程字段合并，未保存偏好"));
    QString parseFailure;
    const auto transaction = storage->updateFile(
        relativePath(), [&](const util::PluginFileSnapshot &snapshot) {
            QJsonObject object;
            if (snapshot.exists && !snapshot.data.trimmed().isEmpty()) {
                QJsonParseError parseError;
                const auto document = QJsonDocument::fromJson(snapshot.data, &parseError);
                if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                    parseFailure = QStringLiteral("现有偏好文件无法解析（%1），未覆盖")
                                       .arg(parseError.errorString());
                    return util::PluginFileUpdate::failure(parseFailure);
                }
                object = document.object();
            }
            // Only the fields this controller actually edited are written; the
            // rest stay exactly as the transaction just read them.
            if (change.serverAddressDirty) {
                if (address.isEmpty()) object.remove(QLatin1String(AddressKey));
                else object.insert(QLatin1String(AddressKey), address);
            }
            if (change.profileDirty) {
                if (profile.isEmpty()) object.remove(QLatin1String(ProfileKey));
                else object.insert(QLatin1String(ProfileKey), profile);
            }
            return util::PluginFileUpdate::replace(
                QJsonDocument(object).toJson(QJsonDocument::Compact));
        });
    if (!transaction.ok)
        return fail(parseFailure.isEmpty() ? transaction.error : parseFailure);
    if (merged) {
        Values result;
        result.loaded = true;
        const auto document = QJsonDocument::fromJson(transaction.data);
        if (document.isObject()) {
            const auto object = document.object();
            result.serverAddress =
                object.value(QLatin1String(AddressKey)).toString().trimmed();
            result.profile = validProfile(object.value(QLatin1String(ProfileKey)).toString());
        }
        *merged = result;
    }
    return true;
}

} // namespace vusiif::zto::desktop_share
