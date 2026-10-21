#include "SettingsStore.h"

#include "CoordinationLock.h"
#include "StorageContext.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>

namespace vusiif::ztgui::storage {
namespace {
QStringList exactWhitelist()
{
    return {
        QStringLiteral("appearance/theme"),
        QStringLiteral("rendering/disableGpuAcceleration"),
        QStringLiteral("rendering/restartHintShown"),
    };
}

QStringList prefixWhitelist()
{
    return {
        QStringLiteral("names/networks/"),
        QStringLiteral("names/peers/"),
        QStringLiteral("names/moons/"),
        QStringLiteral("plugins/disabled/"),
        QStringLiteral("plugins/retain/"),
        QStringLiteral("window/qml/"),
        QStringLiteral("tables/qml/"),
    };
}

bool whitelisted(const QString &key)
{
    if (exactWhitelist().contains(key)) return true;
    for (const auto &prefix : prefixWhitelist()) {
        if (key.startsWith(prefix) && key.size() > prefix.size()) return true;
    }
    return false;
}

bool readObject(const QString &path, QJsonObject &object)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    object = document.object();
    return true;
}

void writeObject(const QString &path, const QJsonObject &object)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    const auto data = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.open(QIODevice::WriteOnly) && file.write(data) == data.size()) file.commit();
}

QString statusText(QSettings::Status status)
{
    switch (status) {
    case QSettings::NoError: return {};
    case QSettings::AccessError: return QStringLiteral("无法访问设置文件");
    case QSettings::FormatError: return QStringLiteral("设置文件格式错误");
    }
    return QStringLiteral("未知设置错误");
}

QSettings::Format sourceFormat()
{
#ifdef ZEROTIER_TESTING
    // Tests redirect the INI user scope; never touch the real registry.
    return QSettings::IniFormat;
#else
    return QSettings::NativeFormat;
#endif
}
} // namespace

SettingsStore::SettingsStore(const QString &fileName)
    : m_fileName(fileName)
{
    if (m_fileName.isEmpty()) return;
    QDir().mkpath(QFileInfo(m_fileName).absolutePath());
    m_settings = std::make_unique<QSettings>(m_fileName, QSettings::IniFormat);
    m_settings->setAtomicSyncRequired(true);
    // Force a read so a malformed file is reported through status().
    m_settings->allKeys();
    if (m_settings->status() != QSettings::NoError) m_error = statusText(m_settings->status());
}

SettingsStore::~SettingsStore()
{
    if (!m_settings) return;
    // A protected store never holds pending changes; syncing it could still
    // create an empty target that the migration retry would then skip.
    if (!m_writeProtection.isEmpty()) return;
    m_settings->sync();
}

QString SettingsStore::fileName() const { return m_fileName; }
bool SettingsStore::valid() const { return m_settings && m_error.isEmpty(); }

bool SettingsStore::writable() const
{
    if (m_fileName.isEmpty()) return false;
    const QFileInfo file(m_fileName);
    if (file.exists()) return file.isWritable();
    const QFileInfo directory(file.absolutePath());
    return directory.exists() && directory.isWritable();
}

QVariant SettingsStore::value(const QString &key, const QVariant &fallback) const
{
    if (!m_settings) return fallback;
    return m_settings->value(key, fallback);
}

void SettingsStore::setValue(const QString &key, const QVariant &value)
{
    if (!m_writeProtection.isEmpty()) { m_error = m_writeProtection; return; }
    if (!m_settings) { m_error = QStringLiteral("没有可用的设置文件"); return; }
    m_settings->setValue(key, value);
    if (m_settings->status() != QSettings::NoError) m_error = statusText(m_settings->status());
}

void SettingsStore::remove(const QString &key)
{
    if (!m_writeProtection.isEmpty()) { m_error = m_writeProtection; return; }
    if (!m_settings) return;
    m_settings->remove(key);
    if (m_settings->status() != QSettings::NoError) m_error = statusText(m_settings->status());
}

void SettingsStore::protectFromWrites(const QString &reason)
{
    if (reason.isEmpty()) return;
    m_writeProtection = reason;
}

QString SettingsStore::guardStorageMigration(SettingsStore &target, const StorageContext &storage)
{
    if (!storage.settingsMigrationPending()) return {};
    const auto reason = QStringLiteral(
        "共享设置尚未恢复到个人目录，本次运行的宿主设置不会保存（下次启动会自动重试）。");
    target.protectFromWrites(reason);
    return reason;
}

bool SettingsStore::contains(const QString &key) const
{
    return m_settings && m_settings->contains(key);
}

QStringList SettingsStore::allKeys() const
{
    return m_settings ? m_settings->allKeys() : QStringList();
}

bool SettingsStore::sync(QString &error)
{
    if (!m_writeProtection.isEmpty()) { error = m_writeProtection; m_error = error; return false; }
    if (!m_settings) { error = QStringLiteral("没有可用的设置文件"); return false; }
    m_settings->sync();
    error = statusText(m_settings->status());
    if (!error.isEmpty()) m_error = error;
    return error.isEmpty();
}

bool SettingsStore::validIniFile(const QString &path, QString &error)
{
    if (!QFileInfo::exists(path)) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
    const auto bytes = file.readAll();
    file.close();
    if (bytes.contains('\0')) { error = QStringLiteral("设置文件包含 NUL 字节"); return false; }
    const auto decoded = QString::fromUtf8(bytes);
    if (decoded.toUtf8() != bytes) { error = QStringLiteral("设置文件不是有效的 UTF-8"); return false; }
    const auto lines = decoded.split(QLatin1Char('\n'));
    bool section = false;
    for (const auto &raw : lines) {
        const auto line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char(';')) || line.startsWith(QLatin1Char('#'))) continue;
        if (line.startsWith(QLatin1Char('['))) {
            if (!line.endsWith(QLatin1Char(']'))) { error = QStringLiteral("设置文件的分节头不完整"); return false; }
            section = true;
            continue;
        }
        const auto equals = line.indexOf(QLatin1Char('='));
        if (!section || equals <= 0) { error = QStringLiteral("设置文件包含无效条目"); return false; }
        // Control characters are never valid in an INI key.
        const auto key = line.left(equals).trimmed();
        for (const auto character : key) {
            if (character.unicode() < 32) { error = QStringLiteral("设置文件包含无效键名"); return false; }
        }
    }
    QSettings settings(path, QSettings::IniFormat);
    settings.allKeys();
    error = statusText(settings.status());
    return error.isEmpty();
}

SettingsMigrationResult SettingsStore::migrateLegacy(SettingsStore &target,
                                                     const QString &metadataDirectory,
                                                     const QString &userKey)
{
    SettingsMigrationResult result;
    const QString markerPath = QDir(metadataDirectory).filePath(QStringLiteral("settings-migration.json"));
    if (!target.writeProtection().isEmpty()) {
        // The shared-root copy of this very file is still pending: importing
        // defaults or writing the completion marker would fix the wrong state.
        result.complete = false;
        result.error = target.writeProtection();
        return result;
    }
    if (metadataDirectory.isEmpty()) {
        result.complete = false;
        result.error = QStringLiteral("没有存储元信息目录，已保留旧设置待下次重试");
        return result;
    }
    // The completion marker, source keys and target file are one transaction
    // across processes. A short timeout keeps startup responsive; failure keeps
    // the sources and never records completion.
    CoordinationLock coordination(metadataDirectory);
    QString lockError;
    if (!coordination.acquire(lockError, 2000)) {
        result.complete = false;
        result.error = QStringLiteral("设置迁移正在被其他进程处理，已保留旧设置待下次重试：%1").arg(lockError);
        return result;
    }
    QJsonObject marker;
    if (readObject(markerPath, marker) && marker.value("completed").toBool()) {
        result.alreadyCompleted = true;
        return result;
    }
    if (!target.valid() || !target.writable()) {
        result.complete = false;
        result.error = target.error().isEmpty()
            ? QStringLiteral("设置文件不可写：%1").arg(target.fileName())
            : target.error();
        return result;
    }
    QString formatError;
    if (!validIniFile(target.fileName(), formatError)) {
        result.complete = false;
        result.error = QStringLiteral("%1：%2").arg(target.fileName(), formatError);
        return result;
    }

    result.attempted = true;
    const auto format = sourceFormat();
    QSettings current(format, QSettings::UserScope, QStringLiteral("ZeroTierGUI"), QStringLiteral("ZTGUI"));
    QSettings previous(format, QSettings::UserScope, QStringLiteral("ZeroTierGUI"), QStringLiteral("ZeroTier GUI"));
    QSettings *sources[]{&current, &previous};
    bool sourceFailure = false;
    for (auto *source : sources) {
        source->allKeys();
        if (source->status() == QSettings::NoError) continue;
        // A missing or empty historical source is not an error. Access problems
        // must not be recorded as a completed migration.
        if (source->status() != QSettings::NoError) {
            sourceFailure = true;
            result.warnings.append(QStringLiteral("无法读取旧设置来源：%1").arg(statusText(source->status())));
        }
    }

    QStringList candidates;
    for (auto *source : sources) for (const auto &key : source->allKeys())
        if (whitelisted(key) && !candidates.contains(key)) candidates.append(key);

    for (const auto &key : candidates) {
        if (target.contains(key)) continue;
        QSettings *source = nullptr;
        QVariant value;
        for (auto *candidate : sources) {
            if (!candidate->contains(key)) continue;
            source = candidate;
            value = candidate->value(key);
            break;
        }
        if (!source) continue;
        target.setValue(key, value);
        QString error;
        if (!target.sync(error)) {
            result.complete = false;
            result.warnings.append(QStringLiteral("无法写入设置 %1：%2").arg(key, error));
            continue;
        }
        if (target.value(key) != value) {
            result.complete = false;
            result.warnings.append(QStringLiteral("设置 %1 写入后校验失败，旧值保留").arg(key));
            continue;
        }
        // The target is now authoritative; removing the source is best effort.
        source->remove(key);
        source->sync();
        if (source->status() != QSettings::NoError)
            result.warnings.append(QStringLiteral("无法清除旧设置 %1").arg(key));
        ++result.imported;
    }
    if (sourceFailure) result.complete = false;
    QJsonObject updated{{"format", 1},
                        {"completed", result.complete},
                        {"imported", result.imported},
                        {"sourceUser", userKey},
                        {"updatedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    if (!result.warnings.isEmpty()) updated["warnings"] = QJsonArray::fromStringList(result.warnings);
    writeObject(markerPath, updated);
    return result;
}

} // namespace vusiif::ztgui::storage
