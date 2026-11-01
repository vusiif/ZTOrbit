#include "storage/PluginDataStore.h"
#include "storage/SettingsStore.h"
#include "storage/StorageContext.h"
#include "storage/StorageMaintenance.h"
#include "storage/StorageMaintenanceScheduler.h"
#include "AppLog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <atomic>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace vusiif::zto::storage;

class StorageTests : public QObject {
    Q_OBJECT
    QTemporaryDir m_settings;
    void clearLegacySources()
    {
        QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ZeroTierGUI"),
                  QStringLiteral("ZTGUI")).clear();
        QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ZeroTierGUI"),
                  QStringLiteral("ZeroTier GUI")).clear();
    }
    static QJsonObject readJson(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }
private slots:
    void initTestCase()
    {
        QVERIFY(m_settings.isValid());
        QCoreApplication::setOrganizationName("ZTOrbit-Storage-Tests");
        QCoreApplication::setApplicationName("StorageTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
    }
    void cleanup() { clearLegacySources(); }

    void identifiersAreStableAndSafe()
    {
        const auto id = StorageContext::installKey(QCoreApplication::applicationDirPath());
        QCOMPARE(id.size(), 64);
        QCOMPARE(id, StorageContext::installKey(QCoreApplication::applicationDirPath()));
        QCOMPARE(StorageContext::userKey().size(), 64);
#ifdef Q_OS_WIN
        const auto upper = StorageContext::installKey(QStringLiteral("C:/Program Files/ZTOrbit").toUpper());
        const auto lower = StorageContext::installKey(QStringLiteral("C:/Program Files/ZTOrbit"));
        QCOMPARE(upper, lower);
#endif
        QCOMPARE(StorageContext::pluginDataDirectoryName("Voice-Chat"), QStringLiteral("id-voice-chat"));
        QCOMPARE(StorageContext::pluginDataDirectoryName("CON"), QStringLiteral("id-con"));
        QVERIFY(StorageContext::pluginDataDirectoryName("builtin").isEmpty());
        QVERIFY(StorageContext::pluginDataDirectoryName("../escape").isEmpty());
        QVERIFY(StorageContext::pluginDataDirectoryName("with space").isEmpty());
        QVERIFY(StorageContext::pluginDataDirectoryName(QString(129, 'a')).isEmpty());
    }

    void settingsMigrationPriorityAndWhitelist()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto targetPath = directory.filePath("settings.ini");
        {
            QSettings current(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ZeroTierGUI"),
                              QStringLiteral("ZTGUI"));
            current.setValue("appearance/theme", "dark");
            current.setValue("names/peers/0123456789", QStringLiteral("当前来源"));
            current.setValue("plugins/disabled/example", true);
            current.setValue("voice-chat/volume", 35);
            current.setValue("unknown/key", "keep");
            current.sync();
            QSettings previous(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ZeroTierGUI"),
                               QStringLiteral("ZeroTier GUI"));
            previous.setValue("appearance/theme", "light");
            previous.setValue("rendering/disableGpuAcceleration", false);
            previous.setValue("names/networks/0123456789abcdef", QStringLiteral("历史来源"));
            previous.setValue("window/qml/savedWidth", 1200);
            previous.sync();
        }
        {
            SettingsStore existing(targetPath);
            existing.setValue("appearance/theme", "light");
            QString error;
            QVERIFY(existing.sync(error));
        }
        SettingsStore target(targetPath);
        QVERIFY(target.valid());
        const auto result = SettingsStore::migrateLegacy(target, directory.filePath("meta"), "test-user");
        QVERIFY(result.attempted);
        QVERIFY(result.complete);
        QVERIFY(result.error.isEmpty());
        QCOMPARE(result.imported, 5);
        QCOMPARE(target.value("appearance/theme").toString(), QStringLiteral("light")); // Target wins.
        QCOMPARE(target.value("names/peers/0123456789").toString(), QStringLiteral("当前来源"));
        QCOMPARE(target.value("names/networks/0123456789abcdef").toString(), QStringLiteral("历史来源"));
        QCOMPARE(target.value("rendering/disableGpuAcceleration").toBool(), false);
        QCOMPARE(target.value("window/qml/savedWidth").toInt(), 1200);
        QCOMPARE(target.value("plugins/disabled/example").toBool(), true);
        QVERIFY(!target.contains("voice-chat/volume")); // Plugin business settings stay out of host INI.
        QVERIFY(!target.contains("unknown/key"));
        // Only keys actually imported from a source are removed there.
        QSettings current(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ZeroTierGUI"),
                          QStringLiteral("ZTGUI"));
        QSettings previous(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ZeroTierGUI"),
                           QStringLiteral("ZeroTier GUI"));
        QVERIFY(!current.contains("names/peers/0123456789"));
        QVERIFY(current.contains("voice-chat/volume"));
        QVERIFY(current.contains("unknown/key"));
        QVERIFY(!previous.contains("names/networks/0123456789abcdef"));
        // A conflict skipped at the target must stay in its source.
        QVERIFY(previous.contains("appearance/theme"));
        auto marker = readJson(directory.filePath("meta/settings-migration.json"));
        QCOMPARE(marker.value("completed").toBool(), true);
        QCOMPARE(marker.value("sourceUser").toString(), QStringLiteral("test-user"));
        const auto again = SettingsStore::migrateLegacy(target, directory.filePath("meta"), "test-user");
        QVERIFY(again.alreadyCompleted);
        QCOMPARE(again.imported, 0);
    }

    void settingsMigrationHoldsTheCoordinationLock()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto metadata = directory.filePath("meta");
        {
            QSettings source(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ZeroTierGUI"),
                             QStringLiteral("ZTGUI"));
            source.setValue("appearance/theme", "dark");
            source.setValue("names/peers/0123456789", QStringLiteral("来源备注"));
            source.sync();
        }
        SettingsStore target(directory.filePath("settings.ini"));
        QVERIFY(target.valid());
        CoordinationLock held(metadata);
        QString error;
        QVERIFY2(held.acquire(error), qPrintable(error));
        const auto blocked = SettingsStore::migrateLegacy(target, metadata, "test-user");
        QVERIFY(!blocked.attempted);
        QVERIFY(!blocked.complete);
        QVERIFY(!blocked.error.isEmpty());
        QVERIFY(!QFileInfo::exists(QDir(metadata).filePath("settings-migration.json")));
        QVERIFY(!target.contains("appearance/theme"));
        {
            QSettings source(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ZeroTierGUI"),
                             QStringLiteral("ZTGUI"));
            QVERIFY(source.contains("appearance/theme"));
            QVERIFY(source.contains("names/peers/0123456789"));
        }
        held.release();
        const auto completed = SettingsStore::migrateLegacy(target, metadata, "test-user");
        QVERIFY(completed.complete);
        QCOMPARE(completed.imported, 2);
        QCOMPARE(target.value("appearance/theme").toString(), QStringLiteral("dark"));
        QVERIFY(QFileInfo::exists(QDir(metadata).filePath("settings-migration.json")));
        {
            QSettings source(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ZeroTierGUI"),
                             QStringLiteral("ZTGUI"));
            QVERIFY(!source.contains("appearance/theme"));
            QVERIFY(!source.contains("names/peers/0123456789"));
        }
    }

    void corruptSettingsAreNotOverwritten()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("settings.ini");
        const QByteArray corrupt("[broken\nvalue===\n\xff\xfe");
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(corrupt);
        }
        QString error;
        QVERIFY(!SettingsStore::validIniFile(path, error));
        QVERIFY(!error.isEmpty());
        SettingsStore settings(path);
        QVERIFY(!settings.valid());
        const auto result = SettingsStore::migrateLegacy(settings, directory.filePath("meta"), "test-user");
        QVERIFY(!result.complete);
        QVERIFY(!result.error.isEmpty());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), corrupt);
        QVERIFY(!QFileInfo::exists(directory.filePath("meta/settings-migration.json")));
    }

    void writableApplicationDirectoryStaysShared()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto shared = directory.filePath("application");
        QVERIFY(QDir().mkpath(shared));
        auto storage = StorageContext::resolveForTesting(shared, directory.filePath("personal"));
        QVERIFY(storage.isValid());
        QCOMPARE(storage.mode(), StorageContext::Mode::Shared);
        QCOMPARE(storage.activeRoot(), QDir::cleanPath(QFileInfo(shared).absoluteFilePath()));
        QString error;
        QVERIFY(storage.prepare(error));
        QVERIFY(error.isEmpty());
        QVERIFY(QFileInfo::exists(storage.settingsFile().isEmpty() ? QString() : storage.metadataDirectory()
                                  + QStringLiteral("/profile.json")));
        QVERIFY(!QFileInfo::exists(storage.routeFile()));
    }

    void readOnlyFallbackSurvivesRestoredPermissionsAndMigrates()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // A regular file where the application directory should be makes the
        // lookup deterministically unwritable without depending on ACLs.
        const auto blockedParent = directory.filePath("blocked");
        {
            QFile file(blockedParent);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("not a directory");
        }
        const auto shared = QDir(blockedParent).filePath("application");
        const auto personalBase = directory.filePath("user-data");
        auto storage = StorageContext::resolveForTesting(shared, personalBase);
        QVERIFY(storage.isValid());
        QCOMPARE(storage.mode(), StorageContext::Mode::Personal);
        QVERIFY(storage.personal());
        QCOMPARE(storage.activeRoot(), QDir::cleanPath(QFileInfo(storage.personalRoot()).absoluteFilePath()));
        QString error;
        QVERIFY2(storage.prepare(error), qPrintable(error));
        QVERIFY(QFileInfo::exists(storage.routeFile()));
        QVERIFY(QFileInfo::exists(QDir(storage.metadataDirectory()).filePath("profile.json")));
        const auto route = readJson(storage.routeFile());
        QCOMPARE(route.value("mode").toString(), QStringLiteral("personal"));
        QCOMPARE(route.value("installKey").toString(), StorageContext::installKey(shared));

        // Restoring a writable application directory must not move existing
        // users back to the shared root.
        QVERIFY(QFile::remove(blockedParent));
        QVERIFY(QDir().mkpath(shared));
        auto again = StorageContext::resolveForTesting(shared, personalBase);
        QCOMPARE(again.mode(), StorageContext::Mode::Personal);
        QVERIFY(again.personal());
        QCOMPARE(again.activeRoot(), storage.activeRoot());
    }

    void personalProfileCopiesPersistentSharedData()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto shared = directory.filePath("application");
        const auto personalBase = directory.filePath("user-data");
        QVERIFY(QDir().mkpath(shared));
        QVERIFY(QDir().mkpath(QDir(shared).filePath("plugins-data/id-voice-chat")));
        QVERIFY(QDir().mkpath(QDir(shared).filePath(".zto/plugins")));
        QVERIFY(QDir().mkpath(QDir(shared).filePath("logs")));
        {
            QSettings settings(QDir(shared).filePath("settings.ini"), QSettings::IniFormat);
            settings.setValue("appearance/theme", "dark");
            settings.setValue("names/peers/0123456789", QStringLiteral("共享备注"));
            settings.sync();
        }
        {
            QFile config(QDir(shared).filePath("plugins-data/id-voice-chat/config.json"));
            QVERIFY(config.open(QIODevice::WriteOnly));
            config.write("{\"volume\":35}");
        }
        {
            QFile owner(QDir(shared).filePath(".zto/plugins/voice-chat.json"));
            QVERIFY(owner.open(QIODevice::WriteOnly));
            owner.write("{\"id\":\"voice-chat\",\"names\":[\"语音\"]}");
        }
        {
            QFile log(QDir(shared).filePath("logs/250101-1.log"));
            QVERIFY(log.open(QIODevice::WriteOnly));
            log.write("shared log");
        }
        // Pre-record the personal route, emulating an earlier fallback start.
        const auto routeDirectory = QDir(personalBase).filePath(
            QStringLiteral("installations/") + StorageContext::installKey(shared));
        QVERIFY(QDir().mkpath(routeDirectory));
        {
            QFile route(QDir(routeDirectory).filePath("route.json"));
            QVERIFY(route.open(QIODevice::WriteOnly));
            route.write(QJsonDocument(QJsonObject{{"format", 1}, {"mode", "personal"}}).toJson());
        }
        auto storage = StorageContext::resolveForTesting(shared, personalBase);
        QCOMPARE(storage.mode(), StorageContext::Mode::Personal);
        QString error;
        QVERIFY2(storage.prepare(error), qPrintable(error));
        QVERIFY(QFileInfo::exists(storage.settingsFile()));
        QSettings copied(storage.settingsFile(), QSettings::IniFormat);
        QCOMPARE(copied.value("appearance/theme").toString(), QStringLiteral("dark"));
        QCOMPARE(copied.value("names/peers/0123456789").toString(), QStringLiteral("共享备注"));
        QFile config(storage.pluginDataDirectory("voice-chat") + "/config.json");
        QVERIFY(config.open(QIODevice::ReadOnly));
        QCOMPARE(config.readAll(), QByteArray("{\"volume\":35}"));
        QVERIFY(QFileInfo::exists(QDir(storage.metadataDirectory()).filePath("plugins/voice-chat.json")));
        // Logs stay at the shared root; leases and deletable caches are not copied.
        QVERIFY(QFileInfo::exists(QDir(shared).filePath("logs/250101-1.log")));
        QVERIFY(!QFileInfo::exists(QDir(storage.logsDirectory()).filePath("250101-1.log")));
        QVERIFY(QFileInfo::exists(QDir(storage.metadataDirectory()).filePath("leases")));
        auto migration = readJson(QDir(storage.metadataDirectory()).filePath("migration.json"));
        QCOMPARE(migration.value("completed").toBool(), true);
        QVERIFY(migration.value("items").toArray().size() >= 3);
        // Shared data changing later must not overwrite the personal copy.
        QSettings changed(QDir(shared).filePath("settings.ini"), QSettings::IniFormat);
        changed.setValue("appearance/theme", "light");
        changed.sync();
        auto after = StorageContext::resolveForTesting(shared, personalBase);
        QVERIFY(after.prepare(error));
        QSettings stillCopied(storage.settingsFile(), QSettings::IniFormat);
        QCOMPARE(stillCopied.value("appearance/theme").toString(), QStringLiteral("dark"));
    }

    void pluginDataStoreMigratesLegacyDirectoryAndTracksLeases()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto dataRoot = directory.filePath("plugins-data");
        const auto metadataRoot = directory.filePath(".zto");
        QVERIFY(QDir().mkpath(QDir(dataRoot).filePath(QStringLiteral("旧名字"))));
        {
            QFile config(QDir(dataRoot).filePath(QStringLiteral("旧名字/config.json")));
            QVERIFY(config.open(QIODevice::WriteOnly));
            config.write("{\"volume\":70}");
        }
        PluginDataStore store(dataRoot, metadataRoot, "run-a", "user-a");
        QString error;
        const auto data = store.prepare("Voice-Chat", QStringLiteral("旧名字"), error);
        QVERIFY2(!data.isEmpty(), qPrintable(error));
        error.clear();
        QCOMPARE(QFileInfo(data).fileName(), QStringLiteral("id-voice-chat"));
        QVERIFY(QFileInfo::exists(QDir(data).filePath("config.json")));
        QVERIFY(!QFileInfo::exists(QDir(dataRoot).filePath(QStringLiteral("旧名字"))));
        const auto ownership = store.ownershipRecord("voice-chat", error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(ownership.value("id").toString(), QStringLiteral("Voice-Chat"));
        const auto names = ownership.value("names").toArray();
        QVERIFY(std::any_of(names.cbegin(), names.cend(), [](const QJsonValue &value) {
            return value.toString() == QStringLiteral("旧名字");
        }));

        PluginDataStore other(dataRoot, metadataRoot, "run-b", "user-b");
        QString otherError;
        QVERIFY(!other.prepare("voice-chat", "Voice", otherError).isEmpty());
        QVERIFY(store.hasOtherLeases("voice-chat", error));
        other.release("voice-chat");
        QVERIFY(!store.hasOtherLeases("voice-chat", error));
        // The store's own lease does not block deletion in the owning process.
        QVERIFY(store.deleteDataDirectory("voice-chat", error));
        QVERIFY(!QFileInfo::exists(data));
        // Host-owned metadata survives data deletion (one-way import tombstone).
        QVERIFY(QFileInfo::exists(QDir(metadataRoot).filePath("plugins/voice-chat.json")));
        store.release("voice-chat");
    }

    void legacyDirectoryMigrationWaitsForOtherLeases()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto dataRoot = directory.filePath("plugins-data");
        const auto metadataRoot = directory.filePath(".zto");
        const auto legacy = QDir(dataRoot).filePath(QStringLiteral("旧名字"));
        QVERIFY(QDir().mkpath(legacy));
        {
            QFile config(QDir(legacy).filePath("config.json"));
            QVERIFY(config.open(QIODevice::WriteOnly));
            config.write("{\"volume\":70}");
        }
        const auto otherLease = QDir(metadataRoot).filePath(
            QStringLiteral("leases/id-voice-chat/run-other.json"));
        QVERIFY(QDir().mkpath(QFileInfo(otherLease).absolutePath()));
        {
            QFile lease(otherLease);
            QVERIFY(lease.open(QIODevice::WriteOnly));
            lease.write(QJsonDocument(QJsonObject{{"format", 1}, {"pluginId", "voice-chat"},
                                                  {"runId", "run-other"}}).toJson());
        }
        PluginDataStore store(dataRoot, metadataRoot, "run-a", "user-a");
        QString error;
        const auto data = store.prepare("Voice-Chat", QStringLiteral("旧名字"), error);
        QVERIFY2(!data.isEmpty(), qPrintable(error));
        // The other run blocks the rename; nothing is merged, overwritten or
        // deleted and the target is not pre-created for the next attempt.
        QVERIFY(QFileInfo::exists(legacy));
        QVERIFY(!QFileInfo::exists(QDir(data).filePath("config.json")));
        QVERIFY(!QFileInfo::exists(data));
        QVERIFY(std::any_of(store.warnings().cbegin(), store.warnings().cend(),
                            [](const QString &warning) { return warning.contains(QStringLiteral("租约")); }));

        QVERIFY(QDir(QFileInfo(otherLease).absolutePath()).removeRecursively());
        // The same process may prepare again after the lease disappeared.
        const auto retried = store.prepare("Voice-Chat", QStringLiteral("旧名字"), error);
        QVERIFY2(!retried.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(retried).fileName(), QStringLiteral("id-voice-chat"));
        QVERIFY(!QFileInfo::exists(legacy));
        QFile config(QDir(retried).filePath("config.json"));
        QVERIFY(config.open(QIODevice::ReadOnly));
        QCOMPARE(config.readAll(), QByteArray("{\"volume\":70}"));
        QVERIFY(!store.hasOtherLeases("voice-chat", error));
        store.release("voice-chat");
    }

    void unreadableLeaseBlocksMigrationAndDeletion()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto dataRoot = directory.filePath("plugins-data");
        const auto metadataRoot = directory.filePath(".zto");
        const auto legacy = QDir(dataRoot).filePath(QStringLiteral("旧名字"));
        QVERIFY(QDir().mkpath(legacy));
        {
            QFile config(QDir(legacy).filePath("config.json"));
            QVERIFY(config.open(QIODevice::WriteOnly));
            config.write("{\"volume\":70}");
        }
        const auto leaseFile = QDir(metadataRoot).filePath(
            QStringLiteral("leases/id-voice-chat/run-unknown.json"));
        QVERIFY(QDir().mkpath(QFileInfo(leaseFile).absolutePath()));
        {
            QFile lease(leaseFile);
            QVERIFY(lease.open(QIODevice::WriteOnly));
            lease.write("{broken");
        }
        PluginDataStore store(dataRoot, metadataRoot, "run-a", "user-a");
        QString error;
        // Any unreadable lease is conservatively treated as still in use.
        QVERIFY(store.hasOtherLeases("voice-chat", error));
        QVERIFY(error.contains(QStringLiteral("无法读取")));
        QVERIFY(!store.deleteDataDirectory("voice-chat", error));
        error.clear();
        const auto blocked = store.prepare("voice-chat", QStringLiteral("旧名字"), error);
        QVERIFY2(!blocked.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo::exists(legacy));
        QVERIFY(!QFileInfo::exists(QDir(dataRoot).filePath("id-voice-chat")));
        QVERIFY(std::any_of(store.warnings().cbegin(), store.warnings().cend(),
                            [](const QString &warning) { return warning.contains(QStringLiteral("租约")); }));

        QVERIFY(QFile::remove(leaseFile));
        error.clear();
        const auto migrated = store.prepare("voice-chat", QStringLiteral("旧名字"), error);
        QVERIFY2(!migrated.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo::exists(QDir(migrated).filePath("config.json")));
        QVERIFY(!QFileInfo::exists(legacy));

        // The same conservative rule applies to deletion of the stable-ID data.
        QVERIFY(QDir().mkpath(QFileInfo(leaseFile).absolutePath()));
        {
            QFile lease(leaseFile);
            QVERIFY(lease.open(QIODevice::WriteOnly));
            lease.write("{broken");
        }
        QVERIFY(!store.deleteDataDirectory("voice-chat", error));
        QVERIFY(QFileInfo::exists(migrated));
        QVERIFY(QFile::remove(leaseFile));
        error.clear();
        QVERIFY2(store.deleteDataDirectory("voice-chat", error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(migrated));
        store.release("voice-chat");
    }

    void ambiguousDisplayNameIsNotMerged()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto dataRoot = directory.filePath("plugins-data");
        const auto metadataRoot = directory.filePath(".zto");
        QVERIFY(QDir().mkpath(QDir(dataRoot).filePath("Shared")));
        QVERIFY(QDir().mkpath(QDir(metadataRoot).filePath("plugins")));
        for (const auto &id : {QStringLiteral("alpha"), QStringLiteral("beta")}) {
            QFile file(QDir(metadataRoot).filePath("plugins/" + id + ".json"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(QJsonObject{{"format", 1}, {"id", id},
                {"names", QJsonArray{QStringLiteral("Shared")} }}).toJson());
        }
        PluginDataStore store(dataRoot, metadataRoot, "run", "user");
        QString error;
        const auto data = store.prepare("gamma", QStringLiteral("Shared"), error);
        QVERIFY2(!data.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo::exists(QDir(dataRoot).filePath("Shared")));
        QVERIFY(!store.warnings().isEmpty());
    }

    void legacyDisplayNameNeverEscapesPluginsData()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto dataRoot = directory.filePath("plugins-data");
        const auto metadataRoot = directory.filePath(".zto");
        const auto outside = directory.filePath("outside");
        QVERIFY(QDir().mkpath(outside));
        {
            QFile keep(QDir(outside).filePath("keep.txt"));
            QVERIFY(keep.open(QIODevice::WriteOnly));
            keep.write("outside");
        }
        PluginDataStore store(dataRoot, metadataRoot, "run-a", "user-a");
        QString error;
        // A relative traversal is not a legacy directory name: the data stays
        // outside plugins-data and is never renamed into the plugin's directory.
        const auto data = store.prepare("audit", QStringLiteral("../outside"), error);
        QVERIFY2(!data.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo::exists(QDir(outside).filePath("keep.txt")));
        QVERIFY(!QFileInfo::exists(QDir(data).filePath("keep.txt")));
        QVERIFY(std::any_of(store.warnings().cbegin(), store.warnings().cend(),
                            [](const QString &warning) { return warning.contains(QStringLiteral("单一目录名")); }));
        // An absolute display name is rejected the same way.
        PluginDataStore absolute(dataRoot, metadataRoot, "run-b", "user-b");
        error.clear();
        const auto absoluteData = absolute.prepare("audit-two", outside, error);
        QVERIFY2(!absoluteData.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo::exists(QDir(outside).filePath("keep.txt")));
        QVERIFY(!QFileInfo::exists(QDir(absoluteData).filePath("keep.txt")));
        // A backslash-separated name cannot become a path segment either.
        PluginDataStore windows(dataRoot, metadataRoot, "run-c", "user-c");
        error.clear();
        QVERIFY(!windows.prepare("audit-three", QStringLiteral("..\\outside"), error).isEmpty());
        QVERIFY(QFileInfo::exists(QDir(outside).filePath("keep.txt")));
        store.release("audit");
        absolute.release("audit-two");
        windows.release("audit-three");
    }

    void pendingMigrationProtectsPluginWritesUntilItSettles()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto dataRoot = QDir(directory.path()).filePath("plugins-data");
        const auto metadataRoot = QDir(directory.path()).filePath(".zto");
        const auto migrationPath = QDir(metadataRoot).filePath("migration.json");
        const auto writeMigration = [&](const QByteArray &bytes) -> bool {
            if (!QDir().mkpath(metadataRoot)) return false;
            QFile file(migrationPath);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
            return file.write(bytes) == bytes.size();
        };
        const auto item = [&](const QString &status) {
            return QJsonObject{{"key", QStringLiteral("plugin-data:id-field-probe")},
                               {"kind", QStringLiteral("plugin-data")},
                               {"source", QDir(directory.path()).filePath("shared/plugins-data/id-field-probe")},
                               {"target", QDir(dataRoot).filePath("id-field-probe")},
                               {"status", status}};
        };
        const auto record = [&](bool completed, const QString &status) {
            return QJsonDocument(QJsonObject{{"format", 1}, {"completed", completed},
                                             {"items", QJsonArray{item(status)}}}).toJson();
        };
        PluginDataStore store(dataRoot, metadataRoot, "run-a", "user-a");
        // A partial item protects the target directory and a legacy display
        // name of the same plugin; unrelated IDs keep writing.
        QVERIFY(writeMigration(record(false, QStringLiteral("partial"))));
        QVERIFY(!store.pendingMigration("field-probe", QStringLiteral("FieldProbe")).isEmpty());
        QVERIFY(!store.pendingMigration("field-probe", QStringLiteral("旧名字")).isEmpty());
        QVERIFY(store.pendingMigration("other", QStringLiteral("Other")).isEmpty());
        // Settled items and a completed record release the gate.
        QVERIFY(writeMigration(record(false, QStringLiteral("copied"))));
        QVERIFY(store.pendingMigration("field-probe", QStringLiteral("FieldProbe")).isEmpty());
        QVERIFY(writeMigration(record(true, QStringLiteral("partial"))));
        QVERIFY(store.pendingMigration("field-probe", QStringLiteral("FieldProbe")).isEmpty());
        // A damaged progress record cannot prove the target is safe, so writes
        // stay off instead of letting defaults occupy a future copy target.
        QVERIFY(writeMigration("{broken"));
        QVERIFY(!store.pendingMigration("field-probe", QStringLiteral("FieldProbe")).isEmpty());
        QVERIFY(QFileInfo::exists(migrationPath));
        store.release("field-probe");
    }

    void pluginLegacyImportTombstoneSurvivesDataRemoval()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto dataRoot = directory.filePath("plugins-data");
        const auto metadataRoot = directory.filePath(".zto");
        PluginDataStore store(dataRoot, metadataRoot, "run-a", "user-a");
        QString error;
        QVERIFY(!store.prepare("voice-chat", QStringLiteral("语音"), error).isEmpty());
        auto state = store.legacyImportState("voice-chat");
        QVERIFY2(state.ok(), qPrintable(state.error));
        QVERIFY(state.allowed);
        QVERIFY(store.completeLegacyImport("voice-chat").ok());
        state = store.legacyImportState("voice-chat");
        QVERIFY(!state.allowed);
        QVERIFY(store.completeLegacyImport("voice-chat").ok()); // idempotent
        QVERIFY(store.deleteDataDirectory("voice-chat", error));
        QVERIFY(!store.legacyImportState("voice-chat").allowed);
        // An unreadable metadata record never grants permission by accident.
        {
            QFile file(QDir(metadataRoot).filePath("plugins/voice-chat.json"));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write("{broken");
        }
        state = store.legacyImportState("voice-chat");
        QVERIFY(!state.ok());
        QVERIFY(!state.allowed);
    }

    void maintenanceSeparatesOrphansFromAmbiguousLegacyData()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto root = directory.path();
        const auto dataRoot = QDir(root).filePath("plugins-data");
        QVERIFY(QDir().mkpath(QDir(dataRoot).filePath("id-orphan")));
        QVERIFY(QDir().mkpath(QDir(dataRoot).filePath("id-installed")));
        QVERIFY(QDir().mkpath(QDir(dataRoot).filePath(QStringLiteral("旧名字"))));
        {
            QFile file(QDir(dataRoot).filePath("id-orphan/config.json"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{\"value\":1}");
        }
        {
            QFile file(QDir(dataRoot).filePath("id-installed/config.json"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{\"value\":2}");
        }
        const auto context = StorageContext::forTesting(root);
        StorageMaintenance maintenance(context);
        const auto usage = maintenance.usage();
        QCOMPARE(usage.mode, QStringLiteral("shared"));
        QVERIFY(usage.pluginDataBytes > 0);
        const auto orphans = maintenance.orphans({"installed"});
        QCOMPARE(orphans.size(), 2);
        int deletable = 0, manual = 0;
        for (const auto &item : orphans) {
            if (item.deletable) {
                ++deletable;
                QCOMPARE(item.id, QStringLiteral("orphan"));
            } else {
                ++manual;
                QVERIFY(item.path.contains(QStringLiteral("旧名字")));
            }
        }
        QCOMPARE(deletable, 1);
        QCOMPARE(manual, 1);
        // An unparsable/unauthenticated installed package could own any
        // stable-ID directory; the scan must keep every candidate for manual
        // handling instead of offering it for deletion.
        const auto guarded = maintenance.orphans({"installed"}, true);
        QCOMPARE(guarded.size(), 2);
        for (const auto &item : guarded) {
            QVERIFY(!item.deletable);
            if (item.id == QLatin1String("orphan"))
                QVERIFY(item.reason.contains(QStringLiteral("不确定")));
        }
        // Only the explicitly chosen unowned stable-ID directory is removed.
        const auto report = maintenance.cleanupOrphanData({"orphan"});
        QVERIFY2(report.ok(), qPrintable(report.error));
        QCOMPARE(report.released.size(), 1);
        QVERIFY(!QFileInfo::exists(QDir(dataRoot).filePath("id-orphan")));
        QVERIFY(QFileInfo::exists(QDir(dataRoot).filePath("id-installed")));
        QVERIFY(QFileInfo::exists(QDir(dataRoot).filePath(QStringLiteral("旧名字"))));
        QVERIFY(QFileInfo::exists(QDir(root).filePath(".zto/plugins/orphan.json")));
        QVERIFY(!maintenance.orphans({"installed"}).isEmpty());
    }

    void logAndPluginCacheRetentionKeepsActiveAndRecentFiles()
    {        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto root = directory.path();
        const auto logs = QDir(root).filePath("logs");
        const auto cache = QDir(root).filePath("plugins-data/id-example/cache");
        QVERIFY(QDir().mkpath(logs));
        QVERIFY(QDir().mkpath(cache));
        const auto writeFile = [](const QString &path, const QByteArray &bytes) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) return false;
            return file.write(bytes) == bytes.size();
        };
        const auto oldLog = QDir(logs).filePath("200101-1.log");
        const auto activeLog = QDir(logs).filePath("active.log");
        const auto oldCache = QDir(cache).filePath("old.bin");
        const auto recentCache = QDir(cache).filePath("recent.bin");
        QVERIFY(writeFile(oldLog, "old"));
        QVERIFY(writeFile(activeLog, "active"));
        QVERIFY(writeFile(oldCache, "old"));
        QVERIFY(writeFile(recentCache, "recent"));
        const auto old = QDateTime::currentDateTime().addDays(-31);
        const auto idle = QDateTime::currentDateTime().addDays(-8);
        for (const auto &path : {oldLog, oldCache}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(old, QFileDevice::FileModificationTime));
        }
        QFile cacheFile(oldCache);
        QVERIFY(cacheFile.open(QIODevice::ReadWrite));
        QVERIFY(cacheFile.setFileTime(idle, QFileDevice::FileModificationTime));
        cacheFile.close();
        vusiif::zto::util::log::registerActiveFile(activeLog);
        const auto context = StorageContext::forTesting(root);
        StorageMaintenance maintenance(context);
        // The preview applies the same age/size rules without deleting
        // anything; active logs are never candidates.
        const auto preview = maintenance.previewRetention();
        QCOMPARE(preview.logFiles, 1);
        QCOMPARE(preview.logBytes, qint64(3));
        QCOMPARE(preview.cacheFiles, 1);
        QCOMPARE(preview.cacheBytes, qint64(3));
        QVERIFY(QFileInfo::exists(oldLog));
        QVERIFY(QFileInfo::exists(oldCache));
        const auto logReport = maintenance.cleanupLogs();
        QVERIFY(!QFileInfo::exists(oldLog));
        QVERIFY(QFileInfo::exists(activeLog));
        QVERIFY(logReport.released.contains(oldLog));
        QVERIFY(logReport.skipped.contains(activeLog));
        const auto cacheReport = maintenance.cleanupCaches();
        QVERIFY(!QFileInfo::exists(oldCache));
        QVERIFY(QFileInfo::exists(recentCache));
        QVERIFY(cacheReport.released.contains(oldCache));
        QVERIFY(maintenance.previewRetention().empty());
        vusiif::zto::util::log::unregisterActiveFile(activeLog);
        QCOMPARE(vusiif::zto::util::log::maximumFileBytes(), qint64(10 * 1024 * 1024));
    }

    void personalMigrationRetriesSettledItemsAndCopiesOwnership()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto shared = directory.filePath("application");
        const auto personalBase = directory.filePath("user-data");
        const auto dataSource = QDir(shared).filePath("plugins-data/id-voice-chat");
        QVERIFY(QDir().mkpath(dataSource));
        QVERIFY(QDir().mkpath(QDir(shared).filePath(".zto/plugins")));
        QVERIFY(QDir().mkpath(QDir(shared).filePath("logs")));
        QVERIFY(QDir().mkpath(QDir(shared).filePath("cache")));
        QVERIFY(QDir().mkpath(QDir(shared).filePath(".zto/leases/id-voice-chat")));
        QVERIFY(QDir().mkpath(QDir(shared).filePath(".zto/pending-deletions")));
        {
            QSettings settings(QDir(shared).filePath("settings.ini"), QSettings::IniFormat);
            settings.setValue("appearance/theme", "dark");
            settings.setValue("names/peers/0123456789", QStringLiteral("共享备注"));
            settings.sync();
        }
        const auto write = [](const QString &path, const QByteArray &bytes) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) return false;
            return file.write(bytes) == bytes.size();
        };
        QVERIFY(write(QDir(dataSource).filePath("config.json"), "{\"volume\":35}"));
        QVERIFY(write(QDir(shared).filePath(".zto/plugins/voice-chat.json"),
                      "{\"format\":1,\"id\":\"voice-chat\",\"names\":[\"Voice\"]}"));
        QVERIFY(write(QDir(shared).filePath(".zto/settings-migration.json"),
                      "{\"format\":1,\"completed\":true}"));
        QVERIFY(write(QDir(shared).filePath("logs/250101-1.log"), "shared log"));
        QVERIFY(write(QDir(shared).filePath("cache/session.bin"), "cache"));
        QVERIFY(write(QDir(shared).filePath(".zto/leases/id-voice-chat/run-b.json"), "{}"));
        QVERIFY(write(QDir(shared).filePath(".zto/pending-deletions/old.json"), "{}"));
        // Pre-record the personal route so the selection is deterministic.
        const auto routeDirectory = QDir(personalBase).filePath(
            QStringLiteral("installations/") + StorageContext::installKey(shared));
        QVERIFY(QDir().mkpath(routeDirectory));
        QVERIFY(write(QDir(routeDirectory).filePath("route.json"),
                      QJsonDocument(QJsonObject{{"format", 1}, {"mode", "personal"}}).toJson()));

        auto storage = StorageContext::resolveForTesting(shared, personalBase);
        QCOMPARE(storage.mode(), StorageContext::Mode::Personal);
        QVERIFY(QDir().mkpath(storage.pluginDataDirectory()));
        // A regular file where the migrated directory belongs forces a
        // deterministic partial migration.
        const auto blocker = storage.pluginDataDirectory("voice-chat");
        QVERIFY(write(blocker, "blocked"));
        QString error;
        QVERIFY2(storage.prepare(error), qPrintable(error));
        QVERIFY(!storage.warnings().isEmpty());
        QVERIFY(std::any_of(storage.warnings().cbegin(), storage.warnings().cend(),
                            [](const QString &warning) { return warning.contains(QStringLiteral("下次启动会继续尝试")); }));
        auto migration = readJson(QDir(storage.metadataDirectory()).filePath("migration.json"));
        QCOMPARE(migration.value("completed").toBool(), false);
        QVERIFY(QFileInfo::exists(blocker));
        QVERIFY(!QFileInfo::exists(QDir(storage.pluginDataDirectory("voice-chat")).filePath("config.json")));
        QSettings copied(storage.settingsFile(), QSettings::IniFormat);
        QCOMPARE(copied.value("appearance/theme").toString(), QStringLiteral("dark"));
        QVERIFY(QFileInfo::exists(QDir(storage.metadataDirectory()).filePath("settings-migration.json")));
        // Leases, pending deletes, logs and disposable caches are not copied.
        QVERIFY(!QFileInfo::exists(QDir(storage.metadataDirectory()).filePath("leases/id-voice-chat/run-b.json")));
        QVERIFY(!QFileInfo::exists(QDir(storage.metadataDirectory()).filePath("pending-deletions/old.json")));
        QVERIFY(!QFileInfo::exists(QDir(storage.logsDirectory()).filePath("250101-1.log")));
        QVERIFY(!QFileInfo::exists(QDir(storage.cacheDirectory()).filePath("session.bin")));

        // Remove the blocker and restart: the failed item is retried while
        // already settled items stay untouched.
        QVERIFY(QFile::remove(blocker));
        auto retry = StorageContext::resolveForTesting(shared, personalBase);
        QVERIFY2(retry.prepare(error), qPrintable(error));
        migration = readJson(QDir(retry.metadataDirectory()).filePath("migration.json"));
        QCOMPARE(migration.value("completed").toBool(), true);
        QFile config(QDir(retry.pluginDataDirectory("voice-chat")).filePath("config.json"));
        QVERIFY(config.open(QIODevice::ReadOnly));
        QCOMPARE(config.readAll(), QByteArray("{\"volume\":35}"));
        config.close();
        QVERIFY(!QFileInfo::exists(QDir(retry.logsDirectory()).filePath("250101-1.log")));
        QVERIFY(!QFileInfo::exists(QDir(retry.cacheDirectory()).filePath("session.bin")));

        // A completed record is authoritative: no second copy overwrites or
        // repopulates a target the user changed afterwards.
        QVERIFY(QFile::remove(QDir(retry.pluginDataDirectory("voice-chat")).filePath("config.json")));
        auto settled = StorageContext::resolveForTesting(shared, personalBase);
        QVERIFY(settled.prepare(error));
        QVERIFY(!QFileInfo::exists(QDir(settled.pluginDataDirectory("voice-chat")).filePath("config.json")));
    }

    void personalMigrationSkipsPluginLogsAndDisposableCaches()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto shared = directory.filePath("application");
        const auto personalBase = directory.filePath("user-data");
        const auto pluginSource = QDir(shared).filePath("plugins-data/id-voice-chat");
        QVERIFY(QDir().mkpath(QDir(pluginSource).filePath("cache")));
        QVERIFY(QDir().mkpath(QDir(pluginSource).filePath("logs")));
        const auto write = [](const QString &path, const QByteArray &bytes) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) return false;
            return file.write(bytes) == bytes.size();
        };
        QVERIFY(write(QDir(pluginSource).filePath("config.json"), "{\"volume\":35}"));
        QVERIFY(write(QDir(pluginSource).filePath("cache/throwaway.bin"), "cache"));
        QVERIFY(write(QDir(pluginSource).filePath("logs/old.log"), "log"));
        const auto routeDirectory = QDir(personalBase).filePath(
            QStringLiteral("installations/") + StorageContext::installKey(shared));
        QVERIFY(QDir().mkpath(routeDirectory));
        QVERIFY(write(QDir(routeDirectory).filePath("route.json"),
                      QJsonDocument(QJsonObject{{"format", 1}, {"mode", "personal"}}).toJson()));
        auto storage = StorageContext::resolveForTesting(shared, personalBase);
        QString error;
        QVERIFY2(storage.prepare(error), qPrintable(error));
        QVERIFY(storage.migrationComplete());
        const auto target = storage.pluginDataDirectory("voice-chat");
        // Persistent business data crosses the boundary; plugin logs and the
        // disposable cache stay at the shared root.
        QVERIFY(QFileInfo::exists(QDir(target).filePath("config.json")));
        QVERIFY(!QFileInfo::exists(QDir(target).filePath("cache/throwaway.bin")));
        QVERIFY(!QFileInfo::exists(QDir(target).filePath("logs/old.log")));
        QVERIFY(QFileInfo::exists(QDir(pluginSource).filePath("cache/throwaway.bin")));
        QVERIFY(QFileInfo::exists(QDir(pluginSource).filePath("logs/old.log")));
        const auto migration = readJson(QDir(storage.metadataDirectory()).filePath("migration.json"));
        QCOMPARE(migration.value("completed").toBool(), true);
        bool sawExcluded = false;
        for (const auto &value : migration.value("items").toArray()) {
            const auto item = value.toObject();
            if (item.value("key").toString() != QLatin1String("plugin-data:id-voice-chat")) continue;
            QCOMPARE(item.value("status").toString(), QStringLiteral("copied"));
            // Only files are counted; both cache/throwaway.bin and logs/old.log
            // are excluded from the copy scope.
            QCOMPARE(item.value("excluded").toInt(), 2);
            sawExcluded = true;
        }
        QVERIFY(sawExcluded);
    }

    void personalMigrationMarksMissingSources()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto shared = directory.filePath("application");
        const auto personalBase = directory.filePath("user-data");
        QVERIFY(QDir().mkpath(shared));
        const auto routeDirectory = QDir(personalBase).filePath(
            QStringLiteral("installations/") + StorageContext::installKey(shared));
        QVERIFY(QDir().mkpath(routeDirectory));
        QFile route(QDir(routeDirectory).filePath("route.json"));
        QVERIFY(route.open(QIODevice::WriteOnly));
        route.write(QJsonDocument(QJsonObject{{"format", 1}, {"mode", "personal"}}).toJson());
        route.close();
        auto storage = StorageContext::resolveForTesting(shared, personalBase);
        QString error;
        QVERIFY2(storage.prepare(error), qPrintable(error));
        auto migration = readJson(QDir(storage.metadataDirectory()).filePath("migration.json"));
        QCOMPARE(migration.value("completed").toBool(), true);
        bool sawMissing = false;
        for (const auto &value : migration.value("items").toArray()) {
            const auto item = value.toObject();
            if (item.value("key").toString() == QLatin1String("settings")) {
                QCOMPARE(item.value("status").toString(), QStringLiteral("missing"));
                sawMissing = true;
            }
        }
        QVERIFY(sawMissing);
        QVERIFY(!QFileInfo::exists(storage.settingsFile()));
    }

    void corruptMigrationRecordIsPreserved()
    {        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto shared = directory.filePath("application");
        const auto personalBase = directory.filePath("user-data");
        QVERIFY(QDir().mkpath(shared));
        QVERIFY(QDir().mkpath(QDir(shared).filePath(".zto/plugins")));
        {
            QSettings settings(QDir(shared).filePath("settings.ini"), QSettings::IniFormat);
            settings.setValue("appearance/theme", "dark");
            settings.sync();
        }
        const auto routeDirectory = QDir(personalBase).filePath(
            QStringLiteral("installations/") + StorageContext::installKey(shared));
        QVERIFY(QDir().mkpath(QDir(routeDirectory).filePath("profile/.zto")));
        {
            QFile route(QDir(routeDirectory).filePath("route.json"));
            QVERIFY(route.open(QIODevice::WriteOnly));
            route.write(QJsonDocument(QJsonObject{{"format", 1}, {"mode", "personal"}}).toJson());
            QFile record(QDir(routeDirectory).filePath("profile/.zto/migration.json"));
            QVERIFY(record.open(QIODevice::WriteOnly));
            record.write("{broken");
        }
        auto storage = StorageContext::resolveForTesting(shared, personalBase);
        QString error;
        QVERIFY2(storage.prepare(error), qPrintable(error));
        QVERIFY(std::any_of(storage.warnings().cbegin(), storage.warnings().cend(),
                            [](const QString &warning) { return warning.contains(QStringLiteral("迁移记录损坏")); }));
        QFile record(QDir(storage.metadataDirectory()).filePath("migration.json"));
        QVERIFY(record.open(QIODevice::ReadOnly));
        QCOMPARE(record.readAll(), QByteArray("{broken"));
        // Nothing was migrated or silently replaced.
        QVERIFY(!QFileInfo::exists(storage.settingsFile()));
        QVERIFY(!QFileInfo::exists(QDir(storage.metadataDirectory()).filePath("plugins/voice-chat.json")));
    }

    void settingsMigrationGateBlocksHostIniUntilRetry()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto shared = directory.filePath("application");
        const auto personalBase = directory.filePath("user-data");
        QVERIFY(QDir().mkpath(shared));
        const auto source = QDir(shared).filePath("settings.ini");
        {
            QSettings original(source, QSettings::IniFormat);
            original.setValue("appearance/theme", QStringLiteral("dark"));
            original.setValue("names/networks/test", QStringLiteral("Original Name"));
            original.sync();
            QCOMPARE(original.status(), QSettings::NoError);
        }
        const auto routeDirectory = QDir(personalBase).filePath(
            QStringLiteral("installations/") + StorageContext::installKey(shared));
        QVERIFY(QDir().mkpath(routeDirectory));
        {
            QFile route(QDir(routeDirectory).filePath("route.json"));
            QVERIFY(route.open(QIODevice::WriteOnly));
            route.write(QJsonDocument(QJsonObject{{"format", 1}, {"mode", "personal"}}).toJson());
        }
        // Only the settings item fails: its source cannot be read. The target
        // must stay absent so the retry can still restore the shared values.
#ifdef Q_OS_WIN
        const HANDLE sourceHandle = CreateFileW(reinterpret_cast<LPCWSTR>(source.utf16()),
            GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(sourceHandle != INVALID_HANDLE_VALUE);
#else
        if (::geteuid() == 0) QSKIP("root can read the blocked migration source");
        QVERIFY(QFile::setPermissions(source, QFile::Permissions()));
#endif
        auto storage = StorageContext::resolveForTesting(shared, personalBase);
        QCOMPARE(storage.mode(), StorageContext::Mode::Personal);
        QString error;
        QVERIFY2(storage.prepare(error), qPrintable(error));
        QVERIFY(!storage.migrationComplete());
        QVERIFY(!storage.migrationCorrupt());
        QVERIFY(storage.settingsMigrationPending());
        const auto target = storage.settingsFile();
        QVERIFY(!QFileInfo::exists(target));
        {
            // A normal C++ save during the wait is refused and creates nothing,
            // including through the store destructor.
            SettingsStore guarded(target);
            QVERIFY(!SettingsStore::guardStorageMigration(guarded, storage).isEmpty());
            guarded.setValue("appearance/theme", QStringLiteral("system"));
            QString syncError;
            QVERIFY(!guarded.sync(syncError));
            QVERIFY(syncError.contains(QStringLiteral("尚未恢复")));
        }
        QVERIFY(!QFileInfo::exists(target));
        {
            // The legacy registry import must not write a completion marker for
            // a profile whose shared settings are still missing.
            SettingsStore guarded(target);
            SettingsStore::guardStorageMigration(guarded, storage);
            const auto imported = SettingsStore::migrateLegacy(guarded, storage.metadataDirectory(),
                                                              storage.userKey());
            QVERIFY(!imported.complete);
            QVERIFY(!imported.error.isEmpty());
            QVERIFY(!QFileInfo::exists(QDir(storage.metadataDirectory())
                .filePath("settings-migration.json")));
        }
        QVERIFY(!QFileInfo::exists(target));
#ifdef Q_OS_WIN
        CloseHandle(sourceHandle);
#else
        QVERIFY(QFile::setPermissions(source, QFile::ReadOwner | QFile::WriteOwner
            | QFile::ReadUser | QFile::WriteUser));
#endif
        // The restart retries the copy and restores the original host settings.
        auto retry = StorageContext::resolveForTesting(shared, personalBase);
        QVERIFY2(retry.prepare(error), qPrintable(error));
        QVERIFY(retry.migrationComplete());
        QVERIFY(!retry.settingsMigrationPending());
        SettingsStore copied(retry.settingsFile());
        QVERIFY(SettingsStore::guardStorageMigration(copied, retry).isEmpty());
        QCOMPARE(copied.value("appearance/theme").toString(), QStringLiteral("dark"));
        QCOMPARE(copied.value("names/networks/test").toString(), QStringLiteral("Original Name"));
        // Normal persistence is allowed again after the item settled.
        copied.setValue("appearance/theme", QStringLiteral("system"));
        QString syncError;
        QVERIFY2(copied.sync(syncError), qPrintable(syncError));
        QCOMPARE(SettingsStore(retry.settingsFile()).value("appearance/theme").toString(),
                 QStringLiteral("system"));
    }

    void legacyMigrationWaitsForConfirmableNameOwnership()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto dataRoot = QDir(directory.path()).filePath("plugins-data");
        const auto metadataRoot = QDir(directory.path()).filePath(".zto");
        const auto legacy = QDir(dataRoot).filePath(QStringLiteral("Shared Name"));
        QVERIFY(QDir().mkpath(legacy));
        const auto writeKeep = [](const QString &path) {
            QFile keep(QDir(path).filePath("keep.json"));
            return keep.open(QIODevice::WriteOnly) && keep.write("legacy") == 6;
        };
        QVERIFY(writeKeep(legacy));
        QVERIFY(QDir().mkpath(QDir(metadataRoot).filePath("plugins")));
        const auto corruptRecord = QDir(metadataRoot).filePath("plugins/test-twin-b.json");
        {
            QFile record(corruptRecord);
            QVERIFY(record.open(QIODevice::WriteOnly));
            record.write("{broken");
        }
        // The scan-wide claims name both same-name IDs even though only one
        // ownership record is writable; the failure is reported, never ignored.
        PluginDataStore store(dataRoot, metadataRoot, "run-a", "user-a");
        const auto claims = store.registerInstalledNames(
            {{QStringLiteral("test-twin-a"), QStringLiteral("Shared Name")},
             {QStringLiteral("test-twin-b"), QStringLiteral("Shared Name")}});
        QVERIFY(claims.authoritative);
        QCOMPARE(claims.installed.size(), 2);
        QCOMPARE(claims.issues.size(), 1);
        store.setInstalledNameClaims(claims);
        QString error;
        const auto data = store.prepare("test-twin-a", QStringLiteral("Shared Name"), error);
        QVERIFY2(!data.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo::exists(QDir(legacy).filePath("keep.json")));
        QVERIFY(!QFileInfo::exists(QDir(data).filePath("keep.json")));
        QVERIFY(std::any_of(store.warnings().cbegin(), store.warnings().cend(),
                            [](const QString &warning) { return warning.contains(QStringLiteral("归属")); }));
        // The corrupt record itself is preserved for manual review.
        QFile preserved(corruptRecord);
        QVERIFY(preserved.open(QIODevice::ReadOnly));
        QCOMPARE(preserved.readAll(), QByteArray("{broken"));
        store.release("test-twin-a");

        // The in-memory claim alone blocks the rename when the metadata write
        // could not happen (no issues, no readable conflicting record).
        QTemporaryDir memoryOnly;
        QVERIFY(memoryOnly.isValid());
        const auto memoryDataRoot = QDir(memoryOnly.path()).filePath("plugins-data");
        const auto memoryMetadataRoot = QDir(memoryOnly.path()).filePath(".zto");
        const auto memoryLegacy = QDir(memoryDataRoot).filePath(QStringLiteral("Shared Name"));
        QVERIFY(QDir().mkpath(memoryLegacy));
        QVERIFY(writeKeep(memoryLegacy));
        PluginDataStore claimed(memoryDataRoot, memoryMetadataRoot, "run-b", "user-b");
        PluginDataStore::InstalledNameClaims memoryClaims;
        memoryClaims.authoritative = true;
        memoryClaims.installed = {{QStringLiteral("test-twin-a"), QStringLiteral("Shared Name")},
                                  {QStringLiteral("test-twin-b"), QStringLiteral("Shared Name")}};
        claimed.setInstalledNameClaims(memoryClaims);
        error.clear();
        const auto claimedData = claimed.prepare("test-twin-a", QStringLiteral("Shared Name"), error);
        QVERIFY2(!claimedData.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo::exists(QDir(memoryLegacy).filePath("keep.json")));
        QVERIFY(!QFileInfo::exists(QDir(claimedData).filePath("keep.json")));
        claimed.release("test-twin-a");

        // Without a scan-wide list, an unreadable ownership record anywhere in
        // the root still means the legacy directory's owner is unknown.
        QTemporaryDir direct;
        QVERIFY(direct.isValid());
        const auto directDataRoot = QDir(direct.path()).filePath("plugins-data");
        const auto directMetadataRoot = QDir(direct.path()).filePath(".zto");
        const auto directLegacy = QDir(directDataRoot).filePath(QStringLiteral("Shared Name"));
        QVERIFY(QDir().mkpath(directLegacy));
        QVERIFY(writeKeep(directLegacy));
        QVERIFY(QDir().mkpath(QDir(directMetadataRoot).filePath("plugins")));
        {
            QFile record(QDir(directMetadataRoot).filePath("plugins/other.json"));
            QVERIFY(record.open(QIODevice::WriteOnly));
            record.write("{broken");
        }
        PluginDataStore conservative(directDataRoot, directMetadataRoot, "run-c", "user-c");
        error.clear();
        const auto directData = conservative.prepare("test-twin-a", QStringLiteral("Shared Name"), error);
        QVERIFY2(!directData.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo::exists(QDir(directLegacy).filePath("keep.json")));
        QVERIFY(!QFileInfo::exists(QDir(directData).filePath("keep.json")));
        conservative.release("test-twin-a");
    }

    void retentionEnforcesByteLimits()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto context = StorageContext::forTesting(directory.path());
        QVERIFY(QDir().mkpath(context.logsDirectory()));
        const auto logs = QDir(context.logsDirectory());
        const auto sparse = [](const QString &path, qint64 bytes) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) return false;
            return file.resize(bytes);
        };
        const auto touch = [](const QString &path, const QDateTime &when) {
            QFile file(path);
            if (!file.open(QIODevice::ReadWrite)) return false;
            return file.setFileTime(when, QFileDevice::FileModificationTime);
        };
        const auto oldLog = logs.filePath("250101-1.log");
        const auto newLog = logs.filePath("250102-1.log");
        QVERIFY(sparse(oldLog, 60LL * 1024 * 1024));
        QVERIFY(sparse(newLog, 60LL * 1024 * 1024));
        QVERIFY(touch(oldLog, QDateTime::currentDateTime().addSecs(-3600)));
        QVERIFY(touch(newLog, QDateTime::currentDateTime().addSecs(-60)));
        const auto cacheDirectory = QDir(context.pluginDataDirectory("example")).filePath("cache");
        QVERIFY(QDir().mkpath(cacheDirectory));
        const auto oldCache = QDir(cacheDirectory).filePath("old.bin");
        const auto newCache = QDir(cacheDirectory).filePath("new.bin");
        QVERIFY(sparse(oldCache, 130LL * 1024 * 1024));
        QVERIFY(sparse(newCache, 130LL * 1024 * 1024));
        QVERIFY(touch(oldCache, QDateTime::currentDateTime().addSecs(-7200)));
        QVERIFY(touch(newCache, QDateTime::currentDateTime().addSecs(-60)));
        StorageMaintenance maintenance(context);
        const auto logReport = maintenance.cleanupLogs();
        QVERIFY(!QFileInfo::exists(oldLog));
        QVERIFY(QFileInfo::exists(newLog));
        QVERIFY(logReport.released.contains(oldLog));
        const auto cacheReport = maintenance.cleanupCaches();
        QVERIFY(!QFileInfo::exists(oldCache));
        QVERIFY(QFileInfo::exists(newCache));
        QVERIFY(cacheReport.released.contains(oldCache));
    }

    void maintenanceSchedulerDebouncesStartupDailyRotationAndCacheWrites()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto context = StorageContext::forTesting(directory.path());
        StorageMaintenanceScheduler scheduler(context);
        std::atomic<int> both{0}, logs{0}, caches{0};
        scheduler.setCleanupTask([&](bool runLogs, bool runCaches) {
            if (runLogs && runCaches) ++both;
            else if (runLogs) ++logs;
            else if (runCaches) ++caches;
            return CleanupReport{};
        });
        scheduler.setStartupDelayMs(10);
        scheduler.setDailyIntervalMs(80);
        scheduler.setDebounceMs(25);
        scheduler.start();
        QTRY_VERIFY_WITH_TIMEOUT(both.load() >= 1, 2000);
        // A log rotation notifies once after debounce even when it fires twice.
        scheduler.notifyLogRotation();
        scheduler.notifyLogRotation();
        QTRY_COMPARE_WITH_TIMEOUT(logs.load(), 1, 2000);
        // Only cache/ commits are relevant; repeated writes share one pass.
        scheduler.notifyCacheWrite("cache/a.bin");
        scheduler.notifyCacheWrite("plugins-data/id-example/config.json");
        scheduler.notifyCacheWrite("cache/b.bin");
        QTRY_COMPARE_WITH_TIMEOUT(caches.load(), 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(both.load() >= 2, 2000);
        scheduler.stop();
        QTRY_VERIFY(!scheduler.busy());
        const int total = both.load() + logs.load() + caches.load();
        QTest::qWait(150);
        QCOMPARE(both.load() + logs.load() + caches.load(), total);
        QVERIFY(!scheduler.started());
    }
};

QTEST_GUILESS_MAIN(StorageTests)
#include "StorageTests.moc"
