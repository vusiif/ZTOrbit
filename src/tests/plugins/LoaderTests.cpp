#include "../../client/main/plugins/PluginLoader.h"
#include "../../client/main/plugins/PluginRegistry.h"
#include "../../client/main/plugins/HostServicesImpl.h"
#include "../../client/main/PluginImport.h"
#include "../../client/main/storage/StorageContext.h"
#include "../../client/main/storage/StorageMaintenance.h"
#include "PluginPackage.h"
#include <ztgui/PluginInterface.h>
#include <QFile>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QElapsedTimer>
#include <QQuickItem>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QSettings>
#include <QTimer>
#include <QtTest>
#include <algorithm>
using namespace vusiif::ztgui;
using namespace api;
using namespace plugins;
namespace vusiif::ztgui::package { QString writeLegacy(const QString &,const QString &,QJsonObject); }
class LoaderTests : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
    QString copy(const QString &source,QTemporaryDir &directory) {
        const auto path=directory.filePath(QFileInfo(source).fileName());
        if(!QFile::copy(source,path)) return {}; return path;
    }
    QByteArray trace(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray(); }
    static QJsonObject pendingDeletionRecord(const QString &root, const QString &pluginDirectory,
                                             const QString &dataRoot, const QString &packagePath,
                                             const QString &pendingPath, const QString &stage)
    {
        const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        return QJsonObject{{"format", 1}, {"id", "test-dynamic"}, {"root", root},
                           {"dataRoot", dataRoot}, {"pluginDirectory", pluginDirectory},
                           {"packagePath", packagePath}, {"pendingPath", pendingPath},
                           {"stage", stage}, {"referenced", false},
                           {"createdAt", now}, {"updatedAt", now}};
    }
    static bool writeJson(const QString &path, const QJsonObject &object)
    {
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return false;
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
        return file.write(bytes) == bytes.size();
    }
    static QJsonObject readJson(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }
    api::Result load(PluginLoader &loader,const QString &path,PluginRegistry &registry) {
        QSignalSpy finished(&loader,&PluginLoader::loadFinished);
        const auto accepted=loader.loadFile(path,registry);
        if(!accepted.ok()) return accepted;
        if(finished.isEmpty() && !finished.wait(15000)) return {Error::InvalidPlugin,"Load timed out"};
        const auto result=finished.first();
        return result[1].toBool()?api::Result{}:api::Result{Error::InvalidPlugin,result[2].toString()};
    }
private slots:
    void deleteRequiresUnloadAndRemovesPackageDataAndCatalog() {
        QTemporaryDir directory,data; const auto path=copy(FIXTURE_GOOD,directory);
        QVERIFY(QDir().mkpath(data.filePath("id-test-dynamic/nested")));
        QFile saved(data.filePath("id-test-dynamic/nested/settings.json")); QVERIFY(saved.open(QIODevice::WriteOnly)); saved.write("saved"); saved.close();
        PluginRegistry registry; PluginLoader loader; QVERIFY(load(loader,path,registry).ok());
        QVERIFY(!loader.removeFile(path,directory.path(),data.path()).ok()); QVERIFY(QFileInfo::exists(path));
        QVERIFY(loader.unload("test-dynamic",registry).ok()); QTRY_VERIFY(registry.plugins().isEmpty());
        QTRY_VERIFY(!loader.catalog().first().toMap()["loaded"].toBool());
        QVERIFY(!loader.removeFile(path,data.path(),data.path()).ok());
        const auto removed=loader.removeFile(path,directory.path(),data.path());
        QVERIFY2(removed.ok(),qPrintable(removed.message));
        QVERIFY(!QFileInfo::exists(path)); QVERIFY(!QFileInfo::exists(data.filePath("id-test-dynamic"))); QVERIFY(loader.catalog().isEmpty());
        QVERIFY(!QSettings().contains("plugins/disabled/test-dynamic"));
    }
    void tamperedPackageKeepsDataAndRequiresPackageOnlyRemoval()
    {
        QTemporaryDir directory, data;
        QVERIFY(directory.isValid() && data.isValid());
        const auto path = copy(FIXTURE_GOOD, directory);
        const auto dataRoot = QDir(data.path()).filePath("plugins-data");
        const auto dataDirectory = QDir(dataRoot).filePath("id-test-dynamic");
        const auto metadataDirectory = QDir(data.path()).filePath(".ztgui");
        QVERIFY(QDir().mkpath(dataDirectory));
        {
            QFile config(QDir(dataDirectory).filePath("config.json"));
            QVERIFY(config.open(QIODevice::WriteOnly));
            config.write("{\"keep\":true}");
        }
        PluginRegistry registry;
        PluginLoader loader;
        QVERIFY(load(loader, path, registry).ok());
        QVERIFY(loader.unload("test-dynamic", registry).ok());
        QTRY_VERIFY(registry.plugins().isEmpty());
        // Corrupt the authenticated ciphertext after the package was catalogued.
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.seek(file.size() - 1));
            const auto byte = file.read(1);
            QVERIFY(file.seek(file.size() - 1));
            QVERIFY(file.write(QByteArray(1, byte[0] ^ 1)) == 1);
        }
        loader.scan(directory.path(), registry);
        QTRY_VERIFY(!loader.catalog().isEmpty() && !loader.catalog().first().toMap()["loading"].toBool());
        QVERIFY(!loader.catalog().first().toMap()["authenticated"].toBool());
        const auto plan = loader.deletionPlan(path);
        QVERIFY(!plan.value("dataRemovable").toBool());
        QVERIFY(plan.value("reason").toString().contains(QStringLiteral("认证")));
        const auto refused = loader.removeFile(path, directory.path(), dataRoot);
        QVERIFY(!refused.ok());
        QVERIFY(refused.message.contains(QStringLiteral("未通过认证")));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(QFileInfo::exists(QDir(dataDirectory).filePath("config.json")));
        QVERIFY(!QFileInfo::exists(QDir(metadataDirectory).filePath("plugins/test-dynamic.json")));
        QVERIFY(!QFileInfo::exists(QDir(metadataDirectory).filePath("pending-deletions/test-dynamic.json")));
        // The explicit package-only entry removes just the file.
        const auto removed = loader.removeFile(path, directory.path(), dataRoot,
                                               PluginLoader::Removal::PackageOnly);
        QVERIFY2(removed.ok(), qPrintable(removed.message));
        QVERIFY(removed.message.contains(QStringLiteral("数据")));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(QFileInfo::exists(QDir(dataDirectory).filePath("config.json")));
        QVERIFY(!QFileInfo::exists(QDir(metadataDirectory).filePath("plugins/test-dynamic.json")));
        QVERIFY(loader.catalog().isEmpty());
        QVERIFY(!QSettings().contains("plugins/disabled/test-dynamic"));
    }
    void unparsablePackageMarksOrphanDataUncertain()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto install = QDir(directory.path()).filePath("plugins");
        const auto dataRoot = QDir(directory.path()).filePath("plugins-data");
        QVERIFY(QDir().mkpath(install));
        const auto broken = QDir(install).filePath("broken.ztplugin");
        {
            QFile file(broken);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("not a plugin package");
        }
        const auto orphan = QDir(dataRoot).filePath("id-ghost");
        QVERIFY(QDir().mkpath(orphan));
        {
            QFile config(QDir(orphan).filePath("config.json"));
            QVERIFY(config.open(QIODevice::WriteOnly));
            config.write("{\"keep\":true}");
        }
        PluginRegistry registry;
        PluginLoader loader;
        loader.scan(install, registry);
        QTRY_VERIFY(!loader.catalog().isEmpty() && !loader.catalog().first().toMap()["loading"].toBool());
        QVERIFY(loader.hasUncertainPackages());
        const auto context = storage::StorageContext::forTesting(directory.path());
        storage::StorageMaintenance maintenance(context);
        const auto guarded = maintenance.orphans({}, true);
        QCOMPARE(guarded.size(), 1);
        QCOMPARE(guarded.first().id, QStringLiteral("ghost"));
        QVERIFY(!guarded.first().deletable);
        QVERIFY(guarded.first().reason.contains(QStringLiteral("不确定")));
        QVERIFY(QFileInfo::exists(QDir(orphan).filePath("config.json")));
        // Removing the unparsable package restores a provable orphan decision.
        QVERIFY(QFile::remove(broken));
        loader.scan(install, registry);
        QVERIFY(!loader.hasUncertainPackages());
        const auto unguarded = maintenance.orphans({}, false);
        QCOMPARE(unguarded.size(), 1);
        QVERIFY(unguarded.first().deletable);
        QVERIFY(!unguarded.first().reason.contains(QStringLiteral("不确定")));
    }
    void disabledAndIncompatiblePackagesProtectInstalledData()
    {
        const auto checkProtected = [](const QString &package, const QString &id, bool loadIt) -> QString {
            QTemporaryDir directory;
            if (!directory.isValid()) return QStringLiteral("temporary directory");
            const auto install = QDir(directory.path()).filePath("plugins");
            const auto dataRoot = QDir(directory.path()).filePath("plugins-data");
            if (!QDir().mkpath(install)) return QStringLiteral("create install directory");
            if (!QFile::copy(package, QDir(install).filePath("plugin.ztplugin")))
                return QStringLiteral("copy package");
            const auto dataDirectory = QDir(dataRoot).filePath("id-" + id.toLower());
            if (!QDir().mkpath(dataDirectory)) return QStringLiteral("create data directory");
            QFile saved(QDir(dataDirectory).filePath("config.json"));
            if (!saved.open(QIODevice::WriteOnly)) return QStringLiteral("write data marker");
            saved.write("{\"keep\":true}");
            saved.close();
            if (id == QLatin1String("test-dynamic"))
                QSettings().setValue("plugins/disabled/test-dynamic", true);
            PluginRegistry registry;
            PluginLoader loader;
            if (loadIt) {
                // A concurrent scan of the same root would reject the load as a
                // duplicate; loadFile performs discovery itself.
                QSignalSpy finished(&loader, &PluginLoader::loadFinished);
                if (!loader.loadFile(QDir(install).filePath("plugin.ztplugin"), registry).ok())
                    return QStringLiteral("loadFile rejected");
                if (finished.isEmpty() && !finished.wait(15000)) return QStringLiteral("load timed out");
                if (finished.first()[1].toBool()) return QStringLiteral("fixture unexpectedly loaded");
            } else {
                loader.scan(install, registry);
            }
            QElapsedTimer wait;
            wait.start();
            while ((loader.catalog().isEmpty() || loader.catalog().first().toMap()["loading"].toBool())
                   && wait.elapsed() < 15000) QTest::qWait(10);
            if (loader.catalog().isEmpty()) return QStringLiteral("catalog empty");
            if (loader.catalog().first().toMap()["loading"].toBool()) return QStringLiteral("catalog still loading");
            const auto row = loader.catalog().first().toMap();
            // Disabled, API-incompatible and failed packages still carry an
            // authenticated stable ID and therefore protect their data.
            if (row["id"].toString() != id)
                return QStringLiteral("catalog id '%1'").arg(row["id"].toString());
            if (!row["authenticated"].toBool()) return QStringLiteral("not authenticated");
            if (loader.hasUncertainPackages()) return QStringLiteral("reported uncertainty");
            const auto context = storage::StorageContext::forTesting(directory.path());
            storage::StorageMaintenance maintenance(context);
            if (!maintenance.orphans({id}, false).isEmpty())
                return QStringLiteral("data classified as orphan");
            if (!QFileInfo::exists(QDir(dataDirectory).filePath("config.json")))
                return QStringLiteral("data marker disappeared");
            return {};
        };
        const auto verify = [&](const QString &package, const QString &id, bool loadIt) {
            const auto error = checkProtected(package, id, loadIt);
            QVERIFY2(error.isEmpty(), qPrintable(error));
        };
        verify(FIXTURE_GOOD, QStringLiteral("test-dynamic"), false);
        verify(FIXTURE_OBSOLETE, QStringLiteral("test-dynamic"), false);
        verify(FIXTURE_LIFECYCLE_FAILED, QStringLiteral("test-lifecycle"), true);
    }

    void deleteFailureStagesKeepConsistentState()
    {
        QTemporaryDir directory, data;
        QVERIFY(directory.isValid() && data.isValid());
        const auto path = copy(FIXTURE_GOOD, directory);
        const auto dataRoot = data.filePath("plugins-data");
        const auto dataDirectory = QDir(dataRoot).filePath("id-test-dynamic");
        const auto metadata = QDir(data.path()).filePath(".ztgui");
        const auto record = QDir(metadata).filePath("pending-deletions/test-dynamic.json");
        QVERIFY(QDir().mkpath(dataDirectory));
        {
            QFile config(QDir(dataDirectory).filePath("config.json"));
            QVERIFY(config.open(QIODevice::WriteOnly));
            config.write("{\"keep\":true}");
        }
        PluginRegistry registry;
        PluginLoader loader;
        QVERIFY(load(loader, path, registry).ok());
        QVERIFY(loader.unload("test-dynamic", registry).ok());
        QTRY_VERIFY(registry.plugins().isEmpty());

        // 1) The operation record cannot be written: nothing changes at all.
        QVERIFY(QDir().mkpath(metadata));
        {
            QFile blocker(QDir(metadata).filePath("pending-deletions"));
            QVERIFY(blocker.open(QIODevice::WriteOnly));
            blocker.write("not a directory");
        }
        auto result = loader.removeFile(path, directory.path(), dataRoot);
        QVERIFY(!result.ok());
        QVERIFY(result.message.contains(QStringLiteral("记录")));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(QFileInfo::exists(QDir(dataDirectory).filePath("config.json")));
        QVERIFY(!QFileInfo::exists(record));
        QVERIFY(loader.pendingDeletions().isEmpty());
        QVERIFY(QFile::remove(QDir(metadata).filePath("pending-deletions")));

        // 2) The package cannot be moved into the controlled pending area: the
        // record is dropped again and both the package and data stay in place.
        const auto pendingDirectory = QDir(directory.path()).filePath(".ztgui-pending-deletions");
        {
            QFile blocker(pendingDirectory);
            QVERIFY(blocker.open(QIODevice::WriteOnly));
        }
        result = loader.removeFile(path, directory.path(), dataRoot);
        QVERIFY(!result.ok());
        QVERIFY(result.message.contains(QStringLiteral("待删区")));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(QFileInfo::exists(QDir(dataDirectory).filePath("config.json")));
        QVERIFY(!QFileInfo::exists(record));
        QVERIFY(loader.pendingDeletions().isEmpty());
        QVERIFY(QFile::remove(pendingDirectory));

        // 3) Data deletion fails because the stable-ID path is a link. The
        // package is moved back, the record is dropped and the linked target is
        // never traversed or deleted.
        const auto target = QDir(data.path()).filePath("junction-target");
        QVERIFY(QDir().mkpath(target));
        {
            QFile keep(QDir(target).filePath("keep.json"));
            QVERIFY(keep.open(QIODevice::WriteOnly));
            keep.write("linked");
        }
        QVERIFY(QDir(dataDirectory).removeRecursively());
#ifdef Q_OS_WIN
        const auto junction = QDir::toNativeSeparators(dataDirectory);
        QVERIFY(QProcess::execute("cmd", {"/c", "mklink", "/J", junction,
                                          QDir::toNativeSeparators(target)}) == 0);
#else
        QVERIFY(QFile::link(target, dataDirectory));
#endif
        result = loader.removeFile(path, directory.path(), dataRoot);
        QVERIFY(!result.ok());
        QVERIFY(result.message.contains(QStringLiteral("数据")));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(QFileInfo::exists(QDir(target).filePath("keep.json")));
        QVERIFY(!QFileInfo::exists(record));
        QVERIFY(loader.pendingDeletions().isEmpty());
#ifdef Q_OS_WIN
        QVERIFY(QProcess::execute("cmd", {"/c", "rmdir", junction}) == 0);
#else
        QVERIFY(QFile::remove(dataDirectory));
#endif

        // Once the obstacle is gone the same authorized delete succeeds.
        const auto removed = loader.removeFile(path, directory.path(), dataRoot);
        QVERIFY2(removed.ok(), qPrintable(removed.message));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(record));
    }

    void ownershipRecordMismatchOrCorruptionCancelsFullDelete()
    {
        QTemporaryDir directory, data;
        QVERIFY(directory.isValid() && data.isValid());
        const auto path = copy(FIXTURE_GOOD, directory);
        const auto dataRoot = QDir(data.path()).filePath("plugins-data");
        const auto dataDirectory = QDir(dataRoot).filePath("id-test-dynamic");
        const auto ownershipPath = QDir(data.path()).filePath(".ztgui/plugins/test-dynamic.json");
        QVERIFY(QDir().mkpath(dataDirectory));
        {
            QFile config(QDir(dataDirectory).filePath("config.json"));
            QVERIFY(config.open(QIODevice::WriteOnly));
            config.write("{\"keep\":true}");
        }
        PluginRegistry registry;
        PluginLoader loader;
        QVERIFY(load(loader, path, registry).ok());
        QVERIFY(loader.unload("test-dynamic", registry).ok());
        QTRY_VERIFY(registry.plugins().isEmpty());

        QVERIFY(writeJson(ownershipPath, {{"format", 1}, {"id", "other-plugin"}}));
        auto refused = loader.removeFile(path, directory.path(), dataRoot);
        QVERIFY(!refused.ok());
        QVERIFY(refused.message.contains(QStringLiteral("所有权")));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(QFileInfo::exists(QDir(dataDirectory).filePath("config.json")));
        QVERIFY(!QFileInfo::exists(QDir(data.path()).filePath(".ztgui/pending-deletions/test-dynamic.json")));

        {
            QFile corrupt(ownershipPath);
            QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
            corrupt.write("{broken");
        }
        refused = loader.removeFile(path, directory.path(), dataRoot);
        QVERIFY(!refused.ok());
        QVERIFY(refused.message.contains(QStringLiteral("所有权")));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(QFileInfo::exists(QDir(dataDirectory).filePath("config.json")));

        // A missing record is allowed, but the one-way tombstone is written
        // before the package leaves the install directory.
        QVERIFY(QFile::remove(ownershipPath));
        const auto removed = loader.removeFile(path, directory.path(), dataRoot);
        QVERIFY2(removed.ok(), qPrintable(removed.message));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(dataDirectory));
        const auto ownership = readJson(ownershipPath);
        QCOMPARE(ownership.value("id").toString(), QStringLiteral("test-dynamic"));
        QCOMPARE(ownership.value("legacyImport").toObject().value("completed").toBool(), true);
    }
    void deleteAndImportUseTheSamePackageChangeLock()
    {
        QTemporaryDir directory, data;
        QVERIFY(directory.isValid() && data.isValid());
        const auto path = copy(FIXTURE_GOOD, directory);
        const auto dataRoot = data.filePath("plugins-data");
        const auto dataDirectory = QDir(dataRoot).filePath("id-test-dynamic");
        QVERIFY(QDir().mkpath(dataDirectory));
        QFile saved(QDir(dataDirectory).filePath("config.json"));
        QVERIFY(saved.open(QIODevice::WriteOnly));
        saved.write("{\"keep\":true}");
        saved.close();
        PluginRegistry registry;
        PluginLoader loader;
        QVERIFY(load(loader, path, registry).ok());
        QVERIFY(loader.unload("test-dynamic", registry).ok());
        QTRY_VERIFY(registry.plugins().isEmpty());
        QTRY_VERIFY(!loader.catalog().first().toMap()["loaded"].toBool());

        const auto marker = directory.filePath("lock-marker");
        QProcess probe;
        probe.start(QStringLiteral(PACKAGE_LOCK_PROBE),
                    {directory.path(), marker, QStringLiteral("4000")});
        QVERIFY(probe.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 5000);

        QElapsedTimer elapsed;
        elapsed.start();
        const auto blocked = loader.removeFile(path, directory.path(), dataRoot);
        QVERIFY(!blocked.ok());
        QVERIFY(elapsed.elapsed() < 1500);
        QVERIFY(blocked.message.contains(QStringLiteral("导入或删除")));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(QFileInfo::exists(QDir(dataDirectory).filePath("config.json")));
        QVERIFY(QSettings().value("plugins/disabled/test-dynamic").toBool());

        const auto sourceDirectory = directory.filePath("source");
        QVERIFY(QDir().mkpath(sourceDirectory));
        const auto source = QDir(sourceDirectory).filePath("other.ztplugin");
        QVERIFY(QFile::copy(FIXTURE_GOOD, source));
        QVERIFY(!importPlugin(source, directory.path()).isEmpty());
        QVERIFY(!QFileInfo::exists(QDir(directory.path()).filePath("other.ztplugin")));

        QVERIFY(probe.waitForFinished(15000));
        QCOMPARE(probe.exitCode(), 0);
        const auto removed = loader.removeFile(path, directory.path(), dataRoot);
        QVERIFY2(removed.ok(), qPrintable(removed.message));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(dataDirectory));
    }

    void generatedDeletionRecordCompletesOnRestart()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory, data;
        QVERIFY(directory.isValid() && data.isValid());
        const auto path = copy(FIXTURE_GOOD, directory);
        const auto dataRoot = QDir(data.path()).filePath("plugins-data");
        const auto dataDirectory = QDir(dataRoot).filePath("id-test-dynamic");
        const auto metadata = QDir(data.path()).filePath(".ztgui");
        const auto record = QDir(metadata).filePath("pending-deletions/test-dynamic.json");
        QVERIFY(QDir().mkpath(dataDirectory));
        {
            QFile config(QDir(dataDirectory).filePath("config.json"));
            QVERIFY(config.open(QIODevice::WriteOnly));
            config.write("{\"keep\":true}");
        }
        PluginRegistry registry;
        PluginLoader loader;
        QVERIFY(load(loader, path, registry).ok());
        QVERIFY(loader.unload("test-dynamic", registry).ok());
        QTRY_VERIFY(registry.plugins().isEmpty());
        QTRY_VERIFY(!loader.catalog().first().toMap()["loaded"].toBool());
        // A read-only package can be moved into the controlled pending area but
        // cannot be deleted, so the normal deletion path leaves a real record.
        QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::ReadUser));
        const auto result = loader.removeFile(path, directory.path(), dataRoot);
        QVERIFY(!result.ok());
        QVERIFY(result.message.contains(QStringLiteral("待删包")));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(dataDirectory));
        QVERIFY(QFileInfo::exists(record));
        const auto pendingRecord = readJson(record);
        // The record must name the plugin data root itself, not its parent, or
        // the recovery entry point rejects its own output.
        QCOMPARE(QDir::cleanPath(pendingRecord.value("dataRoot").toString()), QDir::cleanPath(dataRoot));
        QVERIFY(QFileInfo::exists(QDir(metadata).filePath("plugins/test-dynamic.json")));
        const auto pending = pendingRecord.value("pendingPath").toString();
        QVERIFY(QFileInfo::exists(pending));
        QVERIFY(QFile::setPermissions(pending, QFile::ReadOwner | QFile::WriteOwner
            | QFile::ReadUser | QFile::WriteUser));
        // The restart path consumes the record that removeFile() produced.
        PluginLoader restart;
        QCOMPARE(restart.recoverPendingDeletions(directory.path(), dataRoot), 1);
        QVERIFY(restart.pendingDeletionIssues().isEmpty());
        QVERIFY(!QFileInfo::exists(pending));
        QVERIFY(!QFileInfo::exists(record));
        QVERIFY(QFileInfo::exists(QDir(metadata).filePath("plugins/test-dynamic.json")));
#else
        QSKIP("read-only package files keep the final delete pending on Windows only");
#endif
    }

    void recoveryHonorsPackageLockAndReinstalledReferences()
    {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const auto root = base.filePath("case");
        const auto pluginDirectory = QDir(root).filePath("plugins");
        const auto dataRoot = QDir(root).filePath("plugins-data");
        const auto metadataRoot = QDir(root).filePath(".ztgui");
        const auto package = QDir(pluginDirectory).filePath("plugin.ztplugin");
        const auto pending = QDir(pluginDirectory).filePath(".ztgui-pending-deletions/plugin.ztplugin");
        const auto recordPath = QDir(metadataRoot).filePath("pending-deletions/test-dynamic.json");
        const auto dataDirectory = QDir(dataRoot).filePath("id-test-dynamic");
        const auto createMovedCase = [&](bool reinstall) {
            QVERIFY(QDir(root).removeRecursively());
            QVERIFY(QDir().mkpath(pluginDirectory));
            QVERIFY(QDir().mkpath(dataRoot));
            QVERIFY(QDir().mkpath(QFileInfo(pending).absolutePath()));
            QVERIFY(QFile::copy(FIXTURE_GOOD, pending));
            QVERIFY(QDir().mkpath(dataDirectory));
            QFile saved(QDir(dataDirectory).filePath("config.json"));
            QVERIFY(saved.open(QIODevice::WriteOnly));
            saved.write("{\"keep\":true}");
            saved.close();
            if (reinstall)
                QVERIFY(QFile::copy(FIXTURE_GOOD, QDir(pluginDirectory).filePath("reinstalled.ztplugin")));
            QVERIFY(writeJson(recordPath, pendingDeletionRecord(root, pluginDirectory, dataRoot, package,
                                                                pending, "moved")));
        };
        {
            // Another profile importing into the same directory holds the
            // install-directory lock: recovery must not touch anything.
            createMovedCase(false);
            const auto marker = QDir(root).filePath("lock-marker");
            QProcess probe;
            probe.start(QStringLiteral(PACKAGE_LOCK_PROBE), {pluginDirectory, marker, QStringLiteral("4000")});
            QVERIFY(probe.waitForStarted(5000));
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 5000);
            PluginLoader loader;
            QCOMPARE(loader.recoverPendingDeletions(pluginDirectory, dataRoot), 0);
            QVERIFY(QFileInfo::exists(pending));
            QVERIFY(QFileInfo::exists(QDir(dataDirectory).filePath("config.json")));
            QVERIFY(QFileInfo::exists(recordPath));
            QVERIFY(!loader.pendingDeletionIssues().isEmpty());
            QVERIFY(probe.waitForFinished(15000));
            QCOMPARE(probe.exitCode(), 0);
            // Once the lock is free the same record completes.
            QCOMPARE(loader.recoverPendingDeletions(pluginDirectory, dataRoot), 1);
            QVERIFY(!QFileInfo::exists(pending));
            QVERIFY(!QFileInfo::exists(dataDirectory));
            QVERIFY(!QFileInfo::exists(recordPath));
        }
        {
            // A package installed after the record was written references the
            // same data: finish the authorized package delete, keep the data.
            createMovedCase(true);
            PluginLoader loader;
            QCOMPARE(loader.recoverPendingDeletions(pluginDirectory, dataRoot), 1);
            QVERIFY(QFileInfo::exists(QDir(dataDirectory).filePath("config.json")));
            QVERIFY(QFileInfo::exists(QDir(pluginDirectory).filePath("reinstalled.ztplugin")));
            QVERIFY(!QFileInfo::exists(pending));
            QVERIFY(!QFileInfo::exists(recordPath));
        }
        {
            // A reinstall at the authorized path replaced the package; the hash
            // recorded at authorization time no longer matches, so nothing is
            // moved or deleted.
            createMovedCase(false);
            QVERIFY(QFile::remove(pending));
            QVERIFY(QFile::copy(FIXTURE_GOOD, package));
            auto record = pendingDeletionRecord(root, pluginDirectory, dataRoot, package, pending, "recorded");
            record["packageHash"] = QString(64, QLatin1Char('0'));
            QVERIFY(writeJson(recordPath, record));
            PluginLoader loader;
            QCOMPARE(loader.recoverPendingDeletions(pluginDirectory, dataRoot), 0);
            QVERIFY(QFileInfo::exists(package));
            QVERIFY(QFileInfo::exists(QDir(dataDirectory).filePath("config.json")));
            QVERIFY(QFileInfo::exists(recordPath));
        }
    }

    void sameNameInstallsDoNotClaimLegacyData()
    {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const auto root = base.path();
        const auto pluginDirectory = QDir(root).filePath("plugins");
        const auto dataRoot = QDir(root).filePath("plugins-data");
        const auto legacy = QDir(dataRoot).filePath("Shared Name");
        QVERIFY(QDir().mkpath(pluginDirectory));
        QVERIFY(QDir().mkpath(legacy));
        {
            QFile keep(QDir(legacy).filePath("keep.json"));
            QVERIFY(keep.open(QIODevice::WriteOnly));
            keep.write("legacy");
        }
        QVERIFY(QFile::copy(FIXTURE_TWIN_A, QDir(pluginDirectory).filePath("twin-a.ztplugin")));
        QVERIFY(QFile::copy(FIXTURE_TWIN_B, QDir(pluginDirectory).filePath("twin-b.ztplugin")));
        const auto storage = storage::StorageContext::forTesting(root);
        PluginRegistry registry;
        PluginLoader loader;
        loader.setStorage(storage);
        loader.scan(pluginDirectory, registry);
        QElapsedTimer wait;
        wait.start();
        bool catalogued = false;
        while (wait.elapsed() < 15000) {
            const auto rows = loader.catalog();
            catalogued = rows.size() == 2;
            for (const auto &row : rows) catalogued = catalogued && !row.toMap()["loading"].toBool();
            if (catalogued) break;
            QTest::qWait(10);
        }
        QVERIFY(catalogued);
        // The complete authenticated install list is registered before any
        // instance starts, so both IDs already claim the shared display name.
        const auto metadata = storage.metadataDirectory();
        for (const auto &id : {"test-twin-a", "test-twin-b"}) {
            const auto ownership = readJson(QDir(metadata).filePath(QStringLiteral("plugins/") + id + ".json"));
            QCOMPARE(ownership.value("id").toString(), QString::fromLatin1(id));
            bool claimed = false;
            for (const auto &value : ownership.value("names").toArray())
                if (value.toString() == QLatin1String("Shared Name")) claimed = true;
            QVERIFY(claimed);
        }
        // Normal consumer initialization for either ID leaves the ambiguous
        // legacy directory alone instead of letting the first one take it.
        for (const auto &id : {"test-twin-a", "test-twin-b"}) {
            util::HostServicesImpl host(QStringLiteral("Shared Name"), dataRoot, id, metadata,
                                        storage.runId(), storage.userKeyValue());
            QVERIFY2(host.storageReady(), qPrintable(host.storageError()));
            const QDir target(QDir(dataRoot).filePath(QStringLiteral("id-") + QString::fromLatin1(id)));
            QVERIFY(!QFileInfo::exists(target.filePath("keep.json")));
            QVERIFY(QFileInfo::exists(QDir(legacy).filePath("keep.json")));
        }
        QVERIFY(QFileInfo::exists(QDir(legacy).filePath("keep.json")));
    }

    void sameNameScanWithCorruptOwnershipKeepsLegacyData()
    {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const auto root = base.path();
        const auto pluginDirectory = QDir(root).filePath("plugins");
        const auto dataRoot = QDir(root).filePath("plugins-data");
        const auto legacy = QDir(dataRoot).filePath("Shared Name");
        QVERIFY(QDir().mkpath(pluginDirectory));
        QVERIFY(QDir().mkpath(legacy));
        {
            QFile keep(QDir(legacy).filePath("keep.json"));
            QVERIFY(keep.open(QIODevice::WriteOnly));
            keep.write("legacy");
        }
        QVERIFY(QFile::copy(FIXTURE_TWIN_A, QDir(pluginDirectory).filePath("twin-a.ztplugin")));
        QVERIFY(QFile::copy(FIXTURE_TWIN_B, QDir(pluginDirectory).filePath("twin-b.ztplugin")));
        const auto storage = storage::StorageContext::forTesting(root);
        const auto metadata = storage.metadataDirectory();
        const auto corruptRecord = QDir(metadata).filePath("plugins/test-twin-b.json");
        QVERIFY(QDir().mkpath(QFileInfo(corruptRecord).absolutePath()));
        {
            QFile record(corruptRecord);
            QVERIFY(record.open(QIODevice::WriteOnly));
            record.write("{broken");
        }
        PluginRegistry registry;
        PluginLoader loader;
        loader.setStorage(storage);
        loader.scan(pluginDirectory, registry);
        QElapsedTimer wait;
        wait.start();
        bool catalogued = false;
        while (wait.elapsed() < 15000) {
            const auto rows = loader.catalog();
            catalogued = rows.size() == 2;
            for (const auto &row : rows) catalogued = catalogued && !row.toMap()["loading"].toBool();
            if (catalogued) break;
            QTest::qWait(10);
        }
        QVERIFY(catalogued);
        // The authenticated install list is complete even though one ownership
        // record could not be written; the failed registration is reported.
        const auto claims = loader.installedNameClaims();
        QVERIFY(claims.authoritative);
        QCOMPARE(claims.installed.size(), 2);
        QCOMPARE(claims.issues.size(), 1);
        // A normal consumer of the readable ID must keep the ambiguous legacy
        // directory instead of moving it into the first ID that starts. The ID
        // with the corrupt record cannot start at all, which is why the scan
        // list has to decide for both.
        {
            util::HostServicesImpl host(QStringLiteral("Shared Name"), dataRoot, "test-twin-a", metadata,
                                        storage.runId(), storage.userKeyValue(), {}, claims);
            QVERIFY2(host.storageReady(), qPrintable(host.storageError()));
            const QDir target(QDir(dataRoot).filePath(QStringLiteral("id-test-twin-a")));
            QVERIFY(!QFileInfo::exists(target.filePath("keep.json")));
            QVERIFY(QFileInfo::exists(QDir(legacy).filePath("keep.json")));
            QVERIFY(std::any_of(host.storageWarnings().cbegin(), host.storageWarnings().cend(),
                                [](const QString &warning) {
                                    return warning.contains(QStringLiteral("归属"));
                                }));
        }
        {
            util::HostServicesImpl unreachable(QStringLiteral("Shared Name"), dataRoot, "test-twin-b",
                                               metadata, storage.runId(), storage.userKeyValue(), {}, claims);
            QVERIFY(!unreachable.storageReady());
        }
        QVERIFY(QFileInfo::exists(QDir(legacy).filePath("keep.json")));
        // The corrupt record is preserved: it still names what it protects.
        QFile preserved(corruptRecord);
        QVERIFY(preserved.open(QIODevice::ReadOnly));
        QCOMPARE(preserved.readAll(), QByteArray("{broken"));
    }

    void unauthenticatedPackageKeepsLegacyDisplayNameData()
    {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const auto root = base.path();
        const auto pluginDirectory = QDir(root).filePath("plugins");
        const auto dataRoot = QDir(root).filePath("plugins-data");
        const auto legacy = QDir(dataRoot).filePath("Shared Name");
        QVERIFY(QDir().mkpath(pluginDirectory));
        QVERIFY(QDir().mkpath(legacy));
        {
            QFile keep(QDir(legacy).filePath("keep.json"));
            QVERIFY(keep.open(QIODevice::WriteOnly));
            keep.write("legacy");
        }
        QVERIFY(QFile::copy(FIXTURE_TWIN_A, QDir(pluginDirectory).filePath("twin-a.ztplugin")));
        const auto corrupt = QDir(pluginDirectory).filePath("broken.ztplugin");
        {
            QFile package(corrupt);
            QVERIFY(package.open(QIODevice::WriteOnly));
            package.write("not a plugin package");
        }
        const auto storage = storage::StorageContext::forTesting(root);
        const auto metadata = storage.metadataDirectory();
        PluginRegistry registry;
        PluginLoader loader;
        loader.setStorage(storage);
        loader.scan(pluginDirectory, registry);
        // The unauthenticated package has no trustworthy ID or display name, so
        // its claim on the legacy directory is unknown: the scan says so.
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 15000) {
            const auto claims = loader.installedNameClaims();
            if (claims.authoritative && claims.installed.size() == 1 && !claims.issues.isEmpty()) break;
            QTest::qWait(10);
        }
        const auto claims = loader.installedNameClaims();
        QCOMPARE(claims.installed.size(), 1);
        QVERIFY(!claims.issues.isEmpty());
        // A scan also starts the authenticated in-process package; wait until
        // that load finished so consumer initialization and the next scan see a
        // stable installation instead of a load in progress.
        wait.restart();
        while (wait.elapsed() < 15000) {
            bool loading = false;
            for (const auto &row : loader.catalog()) loading = loading || row.toMap()["loading"].toBool();
            if (!loading) break;
            QTest::qWait(10);
        }
        {
            util::HostServicesImpl host(QStringLiteral("Shared Name"), dataRoot, "test-twin-a", metadata,
                                        storage.runId(), storage.userKeyValue(), {}, claims);
            QVERIFY2(host.storageReady(), qPrintable(host.storageError()));
            QVERIFY(QFileInfo::exists(QDir(legacy).filePath("keep.json")));
            QVERIFY(!QFileInfo::exists(QDir(dataRoot).filePath("id-test-twin-a/keep.json")));
        }
        // Once the unreadable package is gone the same directory migrates.
        QVERIFY(QFile::remove(corrupt));
        loader.scan(pluginDirectory, registry);
        wait.restart();
        while (wait.elapsed() < 15000) {
            const auto refreshed = loader.installedNameClaims();
            if (refreshed.authoritative && refreshed.installed.size() == 1 && refreshed.issues.isEmpty()) break;
            QTest::qWait(10);
        }
        const auto settled = loader.installedNameClaims();
        QVERIFY(settled.issues.isEmpty());
        {
            util::HostServicesImpl host(QStringLiteral("Shared Name"), dataRoot, "test-twin-a", metadata,
                                        storage.runId(), storage.userKeyValue(), {}, settled);
            QVERIFY2(host.storageReady(), qPrintable(host.storageError()));
            QVERIFY(QFileInfo::exists(QDir(dataRoot).filePath("id-test-twin-a/keep.json")));
            QVERIFY(!QFileInfo::exists(legacy));
        }
    }

    void recoveryKeepsPackageWhenLeaseAppearsUnderTheLock()
    {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const auto root = base.filePath("case");
        const auto pluginDirectory = QDir(root).filePath("plugins");
        const auto dataRoot = QDir(root).filePath("plugins-data");
        const auto metadataRoot = QDir(root).filePath(".ztgui");
        const auto package = QDir(pluginDirectory).filePath("plugin.ztplugin");
        const auto pending = QDir(pluginDirectory).filePath(".ztgui-pending-deletions/plugin.ztplugin");
        const auto recordPath = QDir(metadataRoot).filePath("pending-deletions/test-dynamic.json");
        const auto dataDirectory = QDir(dataRoot).filePath("id-test-dynamic");
        const auto leaseFile = QDir(metadataRoot).filePath("leases/id-test-dynamic/run-other.json");
        QVERIFY(QDir().mkpath(pluginDirectory));
        QVERIFY(QDir().mkpath(dataRoot));
        QVERIFY(QFile::copy(FIXTURE_GOOD, package));
        QVERIFY(QDir().mkpath(dataDirectory));
        {
            QFile saved(QDir(dataDirectory).filePath("config.json"));
            QVERIFY(saved.open(QIODevice::WriteOnly));
            saved.write("{\"keep\":true}");
        }
        // Authorized crash point: the record exists, the package was not moved.
        QVERIFY(writeJson(recordPath, pendingDeletionRecord(root, pluginDirectory, dataRoot, package,
                                                            pending, "recorded")));
        // The registrant holds the coordination lock, so recovery waits for it.
        // It writes its lease while still holding the lock, exactly like a
        // loading instance: the lease check, tombstone and package move must
        // therefore happen inside one held lock interval.
        const auto marker = QDir(root).filePath("coordination-held");
        QProcess registrant;
        registrant.start(QStringLiteral(COORDINATION_LOCK_PROBE),
                         {metadataRoot, marker, leaseFile, QStringLiteral("1500")});
        QVERIFY(registrant.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 5000);
        PluginLoader loader;
        const auto recovered = loader.recoverPendingDeletions(pluginDirectory, dataRoot);
        QVERIFY(registrant.waitForFinished(15000));
        QCOMPARE(registrant.exitCode(), 0);
        QCOMPARE(recovered, 0);
        QVERIFY(loader.pendingDeletionIssues().join(QLatin1Char('\n')).contains(QStringLiteral("租约")));
        QVERIFY(QFileInfo::exists(package));
        QVERIFY(!QFileInfo::exists(pending));
        QVERIFY(QFileInfo::exists(QDir(dataDirectory).filePath("config.json")));
        QVERIFY(QFileInfo::exists(recordPath));
        QVERIFY(QFileInfo::exists(leaseFile));
        // After the lease disappears the same authorized record completes.
        QVERIFY(QFile::remove(leaseFile));
        QCOMPARE(loader.recoverPendingDeletions(pluginDirectory, dataRoot), 1);
        QVERIFY(!QFileInfo::exists(package));
        QVERIFY(!QFileInfo::exists(dataDirectory));
        QVERIFY(!QFileInfo::exists(recordPath));
    }

    void pendingDeletionsRecoverEveryStage()
    {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const auto root = base.filePath("case");
        const auto pluginDirectory = QDir(root).filePath("plugins");
        const auto dataRoot = QDir(root).filePath("plugins-data");
        const auto metadataRoot = QDir(root).filePath(".ztgui");
        const auto package = QDir(pluginDirectory).filePath("plugin.ztplugin");
        const auto pending = QDir(pluginDirectory).filePath(".ztgui-pending-deletions/plugin.ztplugin");
        const auto recordPath = QDir(metadataRoot).filePath("pending-deletions/test-dynamic.json");
        const auto dataDirectory = QDir(dataRoot).filePath("id-test-dynamic");
        const auto createCase = [&](const QString &stage, bool original, bool waiting, bool dataPresent,
                                    bool writeRecord, const QString &rootValue = QString()) {
            QVERIFY(QDir(root).removeRecursively());
            QVERIFY(QDir().mkpath(pluginDirectory));
            QVERIFY(QDir().mkpath(dataRoot));
            QVERIFY(QDir().mkpath(metadataRoot));
            if (original) QVERIFY(QFile::copy(FIXTURE_GOOD, package));
            if (waiting) {
                QVERIFY(QDir().mkpath(QFileInfo(pending).absolutePath()));
                QVERIFY(QFile::copy(FIXTURE_GOOD, pending));
            }
            if (dataPresent) {
                QVERIFY(QDir().mkpath(dataDirectory));
                QFile saved(QDir(dataDirectory).filePath("config.json"));
                QVERIFY(saved.open(QIODevice::WriteOnly));
                saved.write("{\"keep\":true}");
            }
            if (writeRecord) {
                auto record = pendingDeletionRecord(rootValue.isEmpty() ? root : rootValue,
                                                    pluginDirectory, dataRoot, package, pending, stage);
                QVERIFY(writeJson(recordPath, record));
            }
        };

        {
            // Crash after the record was written but before the package moved.
            createCase("recorded", true, false, true, true);
            PluginLoader loader;
            QCOMPARE(loader.recoverPendingDeletions(pluginDirectory, dataRoot), 1);
            QVERIFY(!QFileInfo::exists(package));
            QVERIFY(!QFileInfo::exists(dataDirectory));
            QVERIFY(!QFileInfo::exists(recordPath));
        }
        {
            // Crash after the move but before the data was deleted.
            createCase("moved", false, true, true, true);
            PluginLoader loader;
            QCOMPARE(loader.recoverPendingDeletions(pluginDirectory, dataRoot), 1);
            QVERIFY(!QFileInfo::exists(pending));
            QVERIFY(!QFileInfo::exists(dataDirectory));
            QVERIFY(!QFileInfo::exists(recordPath));
        }
        {
            // Data already deleted, only the pending package and record remain.
            createCase("dataDeleted", false, true, false, true);
            PluginLoader loader;
            QCOMPARE(loader.recoverPendingDeletions(pluginDirectory, dataRoot), 1);
            QVERIFY(!QFileInfo::exists(pending));
            QVERIFY(!QFileInfo::exists(recordPath));
        }
        {
            // A live lease blocks further destructive steps and stays visible.
            createCase("moved", false, true, true, true);
            QVERIFY(writeJson(QDir(metadataRoot).filePath("leases/id-test-dynamic/run-live.json"), {}));
            PluginLoader loader;
            QCOMPARE(loader.recoverPendingDeletions(pluginDirectory, dataRoot), 0);
            QCOMPARE(loader.pendingDeletions().size(), 1);
            QVERIFY(!loader.pendingDeletionIssues().isEmpty());
            QVERIFY(QFileInfo::exists(pending));
            QVERIFY(QFileInfo::exists(dataDirectory));
            QVERIFY(QFileInfo::exists(recordPath));
            QVERIFY(QDir(QDir(metadataRoot).filePath("leases")).removeRecursively());
            QCOMPARE(loader.recoverPendingDeletions(pluginDirectory, dataRoot), 1);
            QVERIFY(!QFileInfo::exists(pending));
            QVERIFY(!QFileInfo::exists(dataDirectory));
            QVERIFY(!QFileInfo::exists(recordPath));
        }
        {
            // A live lease also prevents moving the original package while the
            // operation is still in the recorded stage; the record waits.
            createCase("recorded", true, false, true, true);
            QVERIFY(writeJson(QDir(metadataRoot).filePath("leases/id-test-dynamic/run-live.json"), {}));
            PluginLoader loader;
            QCOMPARE(loader.recoverPendingDeletions(pluginDirectory, dataRoot), 0);
            QVERIFY(QFileInfo::exists(package));
            QVERIFY(QFileInfo::exists(dataDirectory));
            QVERIFY(QFileInfo::exists(recordPath));
            QVERIFY(QDir(QDir(metadataRoot).filePath("leases")).removeRecursively());
            QCOMPARE(loader.recoverPendingDeletions(pluginDirectory, dataRoot), 1);
            QVERIFY(!QFileInfo::exists(package));
            QVERIFY(!QFileInfo::exists(dataDirectory));
            QVERIFY(!QFileInfo::exists(recordPath));
        }
        {
            // A record whose pending path is outside the controlled directory
            // is never deleted recursively or moved.
            createCase("moved", false, false, true, false);
            const auto stray = QDir(pluginDirectory).filePath("stray.ztplugin");
            QVERIFY(QFile::copy(FIXTURE_GOOD, stray));
            QVERIFY(writeJson(recordPath, pendingDeletionRecord(root, pluginDirectory, dataRoot, package,
                                                                stray, "moved")));
            PluginLoader loader;
            QCOMPARE(loader.recoverPendingDeletions(pluginDirectory, dataRoot), 0);
            QCOMPARE(loader.pendingDeletions().size(), 1);
            QVERIFY(QFileInfo::exists(stray));
            QVERIFY(QFileInfo::exists(recordPath));
        }
        {
            // Records belong to the root that authorized them.
            createCase("moved", false, true, true, true, base.filePath("other-root"));
            PluginLoader loader;
            QCOMPARE(loader.recoverPendingDeletions(pluginDirectory, dataRoot), 0);
            QVERIFY(loader.pendingDeletions().isEmpty());
            QVERIFY(QFileInfo::exists(pending));
            QVERIFY(QFileInfo::exists(dataDirectory));
            QVERIFY(QFileInfo::exists(recordPath));
        }
        {
            // Both copies existing is ambiguous and must be preserved.
            createCase("recorded", true, true, true, true);
            PluginLoader loader;
            QCOMPARE(loader.recoverPendingDeletions(pluginDirectory, dataRoot), 0);
            QVERIFY(QFileInfo::exists(package));
            QVERIFY(QFileInfo::exists(pending));
            QVERIFY(QFileInfo::exists(dataDirectory));
            QVERIFY(QFileInfo::exists(recordPath));
        }
    }

    void api03PackageIsRejectedBeforeEntry() {        const auto p=package::inspect(FIXTURE_GOOD); QVERIFY(p.ok());
        std::unique_ptr<QTemporaryDir> extracted; std::unique_ptr<QLockFile> lock; QString library;
        QVERIFY(package::extract(p,extracted,library,lock).isEmpty());
        QTemporaryDir temp; const auto path=temp.filePath("api03.ztplugin"),log=temp.filePath("trace");
        auto oldInfo=p.info; oldInfo["apiVersion"]="0.3";
        QVERIFY(package::write(library,path,oldInfo).isEmpty()); qputenv("ZTGUI_PLUGIN_LIFETIME_FILE",log.toUtf8());
        PluginRegistry registry; PluginLoader loader; const auto result=load(loader,path,registry);
        QCOMPARE(result.error,Error::InvalidPlugin); QVERIFY(result.message.contains("API version"));
        QVERIFY(registry.plugins().isEmpty()); QVERIFY(trace(log).isEmpty());
    }
    void ordinaryV1PackageLoadsOnNewHost() {
        auto p=package::inspect(FIXTURE_GOOD); QVERIFY(p.ok());
        std::unique_ptr<QTemporaryDir> extracted; std::unique_ptr<QLockFile> lock; QString library;
        QVERIFY(package::extract(p,extracted,library,lock).isEmpty());
        QTemporaryDir temp; const auto path=temp.filePath("legacy-v1.ztplugin");
        QVERIFY(package::writeLegacy(library,path,p.info).isEmpty());
        PluginRegistry registry; PluginLoader loader; QVERIFY(load(loader,path,registry).ok());
        QCOMPARE(registry.plugins().size(),1); QVERIFY(loader.unload("test-dynamic",registry).ok()); QTRY_VERIFY(registry.plugins().isEmpty());
    }
    void initTestCase() {
        QCoreApplication::setOrganizationName("ZTGUI-Package-Tests"); QCoreApplication::setApplicationName("LoaderTests");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    }
    void cleanup() { QSettings().clear(); qunsetenv("ZTGUI_PLUGIN_DESTROY_THROWS"); qunsetenv("ZTGUI_PLUGIN_NESTED_CALLBACK"); qunsetenv("ZTGUI_PLUGIN_LIFETIME_FILE"); qunsetenv("ZTGUI_PLUGIN_CREATE_STEPS"); qunsetenv("ZTGUI_PLUGIN_PREPARATION_STARTED_FILE"); }
    void validPackageRoundTripAndReload() {
        QTemporaryDir directory; const auto path=copy(FIXTURE_GOOD,directory); QVERIFY(!path.isEmpty());
        PluginRegistry registry; PluginLoader loader; QQmlEngine engine; QQuickItem parent;
        QVERIFY(load(loader,path,registry).ok()); QCOMPARE(registry.plugins().size(),1);
        auto page=registry.page("test-dynamic/overview",&engine,&parent); QVERIFY(page.result.ok()); QPointer<QQuickItem> alive=page.page;
        QCOMPARE(load(loader,path,registry).error,Error::DuplicatePlugin);
        QSignalSpy finished(&loader,&PluginLoader::unloadFinished); QVERIFY(loader.unload("test-dynamic",registry).ok());
        QTRY_COMPARE(finished.size(),1); QVERIFY(finished.first()[1].toBool()); QVERIFY(alive.isNull()); QVERIFY(registry.plugins().isEmpty());
        QVERIFY(QSettings().value("plugins/disabled/test-dynamic").toBool());
        loader.scan(directory.path(),registry); QTRY_VERIFY(!loader.catalog().first().toMap()["loading"].toBool()); QVERIFY(registry.plugins().isEmpty());
        QVERIFY(load(loader,path,registry).ok()); QCOMPARE(registry.plugins().size(),1);
        QVERIFY(!QSettings().contains("plugins/disabled/test-dynamic"));
        loader.requestShutdown(); QTRY_VERIFY(loader.shutdownComplete()); QVERIFY(registry.plugins().isEmpty());
    }
    void largePackageReloadRunsBehindResponsiveEventLoop() {
        const auto original=package::inspect(FIXTURE_GOOD); QVERIFY(original.ok());
        std::unique_ptr<QTemporaryDir> extracted; std::unique_ptr<QLockFile> lock; QString library;
        QVERIFY(package::extract(original,extracted,library,lock).isEmpty());
        QTemporaryDir directory; const auto data=directory.filePath("data"),path=directory.filePath("large.ztplugin");
        QFile resource(data); QVERIFY(resource.open(QIODevice::WriteOnly));
        const QByteArray block(1024*1024,'x');
        for(int i=0;i<64;++i) QCOMPARE(resource.write(block),block.size()); resource.close();
        const QJsonObject manifest{{"files",QJsonArray{
            QJsonObject{{"path","plugin/plugin.dll"},{"role","entry"},{"source",library}},
            QJsonObject{{"path","assets/data.bin"},{"role","resource"},{"source",data}}
        }}};
        QVERIFY(package::writeBundle(manifest,path,original.info).isEmpty());
        PluginRegistry registry; PluginLoader loader;
        QVERIFY(load(loader,path,registry).ok());
        QVERIFY(loader.unload("test-dynamic",registry).ok()); QTRY_VERIFY(registry.plugins().isEmpty());
        QSignalSpy finished(&loader,&PluginLoader::loadFinished);
        int ticks=0; QTimer timer; timer.setInterval(1); connect(&timer,&QTimer::timeout,this,[&] { ++ticks; }); timer.start();
        QElapsedTimer elapsed; elapsed.start(); QVERIFY(loader.loadFile(path,registry).ok()); QVERIFY(elapsed.elapsed()<100);
        QVERIFY(loader.catalog().first().toMap()["loading"].toBool());
        QVERIFY(registry.plugins().isEmpty());
        QCOMPARE(loader.loadFile(path,registry).error,Error::DuplicatePlugin);
        QVERIFY(registry.activateMenu(SettingsMenuId).ok());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,15000); QVERIFY(finished.first()[1].toBool()); QVERIFY(ticks>2);
        QVERIFY(!loader.catalog().first().toMap()["loading"].toBool());
        QVERIFY(registry.activateMenu("test-dynamic/overview").ok());
    }
    void shutdownBeforePackageStartsSkipsPluginEntry() {
        QTemporaryDir directory; const auto path=copy(FIXTURE_GOOD,directory),log=directory.filePath("trace");
        qputenv("ZTGUI_PLUGIN_LIFETIME_FILE",log.toUtf8());
        PluginRegistry registry; PluginLoader loader;
        QSignalSpy finished(&loader,&PluginLoader::loadFinished);
        QVERIFY(loader.loadFile(path,registry).ok()); loader.requestShutdown();
        QTRY_VERIFY(loader.shutdownComplete()); QCOMPARE(finished.size(),1); QVERIFY(!finished.first()[1].toBool());
        QVERIFY(registry.plugins().isEmpty()); QVERIFY(trace(log).isEmpty());
        QVERIFY(!loader.catalog().first().toMap()["loading"].toBool());
    }
    void rawLibrariesAreIgnoredAndOldApiRejected() {
        QTemporaryDir directory; QFile raw(directory.filePath("raw.dll")); QVERIFY(raw.open(QIODevice::WriteOnly)); raw.write("raw"); raw.close();
        PluginRegistry registry; PluginLoader loader; QVERIFY(loader.scan(directory.path(),registry).isEmpty());
        const auto bad=copy(FIXTURE_BAD,directory); QVERIFY(!bad.isEmpty()); QVERIFY(!load(loader,bad,registry).ok());
        const auto old=copy(FIXTURE_OBSOLETE,directory); QVERIFY(!old.isEmpty()); QVERIFY(!load(loader,old,registry).ok());
        const auto legacy=copy(FIXTURE_LEGACY,directory); QVERIFY(!legacy.isEmpty()); QVERIFY(!load(loader,legacy,registry).ok()); QVERIFY(registry.plugins().isEmpty());
    }
    void metadataAndCiphertextTamperingRejected() {
        QTemporaryDir directory; auto path=copy(FIXTURE_GOOD,directory); auto pkg=package::inspect(path); QVERIFY(pkg.ok());
        QCOMPARE(pkg.info["id"].toString(),QString("test-dynamic"));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.seek(file.size()-1));
        const auto byte=file.read(1); QVERIFY(file.seek(file.size()-1)); file.write(QByteArray(1,byte[0]^1)); file.close();
        PluginRegistry registry; PluginLoader loader; QVERIFY(!load(loader,path,registry).ok()); QVERIFY(registry.plugins().isEmpty());
        QFile::remove(path); path=copy(FIXTURE_GOOD,directory); QVERIFY(file.open(QIODevice::ReadWrite));
        auto bytes=file.readAll(); bytes.replace("Dynamic","Altered"); QVERIFY(file.seek(0)); file.write(bytes); file.close();
        QVERIFY(!load(loader,path,registry).ok()); QVERIFY(registry.plugins().isEmpty());
    }
    void incompatibleBuildAndInvalidIdsRejectedBeforeEntry() {
        const auto pkg=package::inspect(FIXTURE_GOOD); QVERIFY(pkg.ok()); auto info=pkg.info;
        for(const auto &key:{"host","platform","architecture","compilerAbi","qtVersion","iid","apiVersion"}) {
            auto changed=info; changed[key]="invalid"; QVERIFY(!package::validate(changed,"client",ApiVersion,ZTGUI_PLUGIN_IID).isEmpty());
        }
        for(const auto &id:{"with.dot","builtin","../escape","中文","with space"}) {
            auto changed=info; changed["id"]=id; QVERIFY(!package::validate(changed,"client",ApiVersion,ZTGUI_PLUGIN_IID).isEmpty());
        }
        PluginRegistry registry; MenuContribution menu; menu.id="overview"; menu.title="Title"; menu.createPage=[](QQmlEngine *,QQuickItem *p) { auto *item=new QQuickItem(p);item->setParent(p);return item; };
        QVERIFY(registry.registerPlugin({{"Mixed-Id","First","1"},{menu}}).ok());
        QCOMPARE(registry.registerPlugin({{"mixed-id","Second","1"},{menu}}).error,Error::DuplicatePlugin);
    }
    void runtimeUnloadDuringInitializationIsResponsive() {
        QTemporaryDir directory; const auto path=copy(FIXTURE_CANCELLABLE_BLOCKED,directory); QVERIFY(!path.isEmpty());
        const auto marker=directory.filePath("started"),log=directory.filePath("trace");
        qputenv("ZTGUI_PLUGIN_PREPARATION_STARTED_FILE",marker.toUtf8()); qputenv("ZTGUI_PLUGIN_LIFETIME_FILE",log.toUtf8());
        PluginRegistry registry; PluginLoader loader; QVERIFY(loader.loadFile(path,registry).ok()); QTRY_VERIFY(QFileInfo::exists(marker));
        int ticks=0; QTimer timer; connect(&timer,&QTimer::timeout,this,[&] { ++ticks; }); timer.start(10);
        QElapsedTimer elapsed; elapsed.start(); QVERIFY(loader.unload("test-cancellable_blocked",registry).ok()); QVERIFY(elapsed.elapsed()<50);
        QCOMPARE(load(loader,path,registry).error,Error::DuplicatePlugin);
        QTRY_VERIFY(registry.plugins().isEmpty()); QVERIFY(ticks>2);
        const auto text=trace(log); QCOMPARE(text.count("destroy\n"),1); QCOMPARE(text.count("entry\n"),1);
        QVERIFY(text.indexOf("cancelled:test-cancellable_blocked")<text.indexOf("destroy"));
        QVERIFY(load(loader,path,registry).ok()); loader.requestShutdown(); QTRY_VERIFY(loader.shutdownComplete());
    }
    void activePageAndFactoriesDieBeforeDestroy() {
        QTemporaryDir directory; const auto path=copy(FIXTURE_LIFECYCLE,directory); const auto log=directory.filePath("trace");
        qputenv("ZTGUI_PLUGIN_LIFETIME_FILE",log.toUtf8()); PluginRegistry registry; PluginLoader loader; QQmlEngine engine; QQuickItem parent;
        QVERIFY(load(loader,path,registry).ok()); QTRY_COMPARE(registry.preparationState("test-lifecycle"),PreparationState::Ready);
        QVERIFY(registry.page("test-lifecycle/overview",&engine,&parent).result.ok()); QVERIFY(registry.activateMenu("test-lifecycle/overview").ok());
        QVERIFY(registry.activateMenu(SettingsMenuId).ok()); QVERIFY(registry.activateMenu("test-lifecycle/overview").ok());
        QVERIFY(loader.unload("test-lifecycle",registry).ok()); QTRY_VERIFY(registry.plugins().isEmpty());
        const auto text=trace(log); QVERIFY(text.contains("exited:builtin/settings"));
        QVERIFY(text.indexOf("exited:builtin/zerotier")<text.indexOf("page\n"));
        QVERIFY(text.indexOf("page\n")<text.indexOf("factory\n")); QVERIFY(text.indexOf("factory\n")<text.indexOf("destroy\n")); QVERIFY(text.indexOf("destroy\n")<text.indexOf("entry\n"));
    }
    void nestedDataCallbackKeepsLibraryAliveUntilReturn() {
        QTemporaryDir directory; const auto path=copy(FIXTURE_LIFECYCLE,directory); const auto log=directory.filePath("trace");
        qputenv("ZTGUI_PLUGIN_LIFETIME_FILE",log.toUtf8()); qputenv("ZTGUI_PLUGIN_NESTED_CALLBACK","1");
        PluginRegistry registry; PluginLoader loader; QVERIFY(load(loader,path,registry).ok()); QTRY_COMPARE(registry.preparationState("test-lifecycle"),PreparationState::Ready);
        QTimer trigger; trigger.setInterval(5); connect(&trigger,&QTimer::timeout,this,[&] {
            if(trace(log).contains("callback-start")) { trigger.stop(); QVERIFY(loader.unload("test-lifecycle",registry).ok()); }
        }); trigger.start();
        QVERIFY(registry.activateMenu("test-lifecycle/overview").ok()); QTRY_VERIFY_WITH_TIMEOUT(registry.plugins().isEmpty(),15000);
        const auto text=trace(log); QVERIFY(text.contains("callback-end")); QVERIFY(text.indexOf("callback-end")<text.indexOf("destroy"));
    }
    void cleanupFailureRetainsLibraryAndPreventsReload() {
        QTemporaryDir directory; const auto path=copy(FIXTURE_LIFECYCLE,directory);
        PluginRegistry registry; PluginLoader loader; QVERIFY(load(loader,path,registry).ok()); QTRY_COMPARE(registry.preparationState("test-lifecycle"),PreparationState::Ready);
        qputenv("ZTGUI_PLUGIN_DESTROY_THROWS","1"); QSignalSpy finished(&loader,&PluginLoader::unloadFinished);
        QVERIFY(loader.unload("test-lifecycle",registry).ok()); QTRY_COMPARE(finished.size(),1);
        QVERIFY(!finished.first()[1].toBool()); QVERIFY(finished.first()[2].toString().contains("cleanup")); QVERIFY(registry.plugins().isEmpty());
        QCOMPARE(load(loader,path,registry).error,Error::DuplicatePlugin); loader.requestShutdown(); QTest::qWait(100); QVERIFY(!loader.shutdownComplete());
    }
    void initializationFailureCleansOnce() {
        QTemporaryDir directory; const auto path=copy(FIXTURE_LIFECYCLE_FAILED,directory); const auto log=directory.filePath("trace"); qputenv("ZTGUI_PLUGIN_LIFETIME_FILE",log.toUtf8());
        PluginRegistry registry; PluginLoader loader; QSignalSpy finished(&loader,&PluginLoader::loadFinished); QVERIFY(loader.loadFile(path,registry).ok());
        QTRY_COMPARE(finished.size(),1); QVERIFY(!finished.first()[1].toBool());
        QTRY_VERIFY(!loader.catalog().first().toMap()["loading"].toBool() && !loader.catalog().first().toMap()["loaded"].toBool());
        QVERIFY(registry.plugins().isEmpty()); QVERIFY(loader.catalog().first().toMap()["message"].toString().contains("failed"));
        const auto text=trace(log); QCOMPARE(text.count("destroy\n"),1); QCOMPARE(text.count("entry\n"),1);
    }
    void shutdownCancelsQueuedTasksAndUnloadsBeforeCompletion() {
        QTemporaryDir directory; const auto log=directory.filePath("trace"); qputenv("ZTGUI_PLUGIN_LIFETIME_FILE",log.toUtf8()); qputenv("ZTGUI_PLUGIN_CREATE_STEPS","1000");
        QVERIFY(QFile::copy(FIXTURE_CANCELLABLE,directory.filePath("a.ztplugin"))); QVERIFY(QFile::copy(FIXTURE_CANCELLABLE_SECOND,directory.filePath("b.ztplugin"))); QVERIFY(QFile::copy(FIXTURE_CANCELLABLE_QUEUED,directory.filePath("c.ztplugin")));
        PluginRegistry registry; PluginLoader loader; loader.scan(directory.path(),registry);
        QTRY_VERIFY(trace(log).contains("started:test-cancellable\n")); QTRY_VERIFY(trace(log).contains("started:test-cancellable_second\n"));
        QTRY_COMPARE(registry.plugins().size(),3);
        QSignalSpy finished(&loader,&PluginLoader::shutdownFinished); loader.requestShutdown(); loader.requestShutdown(); QCOMPARE(finished.size(),0);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,2000); QVERIFY(registry.plugins().isEmpty());
        const auto text=trace(log); QVERIFY(!text.contains("started:test-cancellable_queued")); QCOMPARE(text.count("destroy\n"),3); QCOMPARE(text.count("entry\n"),3);
    }
    void emptyShutdownIsAsynchronousAndIdempotent() {
        PluginLoader loader; QSignalSpy finished(&loader,&PluginLoader::shutdownFinished); loader.requestShutdown(); loader.requestShutdown(); QCOMPARE(finished.size(),0); QTRY_COMPARE(finished.size(),1);
    }
};
QTEST_MAIN(LoaderTests)
#include "LoaderTests.moc"
