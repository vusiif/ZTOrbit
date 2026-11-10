#include <QApplication>
#include <QCoreApplication>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QFont>
#include <QIcon>
#include <QQuickWindow>
#include <cstdio>
#include <cstring>
#include "AppController.h"
#include "AppLog.h"
#include "PluginPackage.h"
#include "SingleInstance.h"
#include "storage/SettingsStore.h"
#include "storage/StorageContext.h"

int main(int argc, char *argv[])
{
    // Internal cleanup mode: no UI, no settings migration, no single instance
    // and no plugin loading. The process waits for its parent, validates the
    // registered ownership token, then reclaims only that cache session.
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--cleanup-plugin-cache") != 0) continue;
        QCoreApplication cleanup(argc, argv);
        QCoreApplication::setOrganizationName("ZTOrbit");
        QCoreApplication::setApplicationName("ZTOrbit");
        return vusiif::zto::package::runCacheCleanupHelper(cleanup.arguments());
    }

    // Qt.labs.platform uses the Widgets backend for native tray menus on Windows.
    QApplication app(argc, argv);
    app.setApplicationName("ZTOrbit");
    app.setApplicationVersion(ZTO_VERSION);
    app.setOrganizationName("ZTOrbit");

    // Select the storage root before reading any host setting. The shared
    // application directory is used while it is actually writable; otherwise
    // this user gets a personal profile and the choice is recorded.
    auto storage = vusiif::zto::storage::StorageContext::resolve();
    QString storageError;
    if (storage.isValid()) storage.prepare(storageError);
    else storageError = storage.error();
    if (!storageError.isEmpty())
        std::fprintf(stderr, "ZTOrbit storage: %s\n", qPrintable(storageError));
    vusiif::zto::util::log::setDirectory(storage.logsDirectory());
    vusiif::zto::package::CacheContext cacheContext;
    cacheContext.root = storage.pluginPackageCacheRoot();
    cacheContext.userKey = storage.userKeyValue();
    cacheContext.runId = storage.runId();
    cacheContext.installKey = storage.installKeyValue();
    cacheContext.legacy = false;
    const auto cacheError = vusiif::zto::package::prepareCacheSession(cacheContext);
    if (!cacheError.isEmpty())
        std::fprintf(stderr, "ZTOrbit cache session: %s\n", qPrintable(cacheError));
    vusiif::zto::package::cleanStaleCache(cacheContext);
    vusiif::zto::storage::SettingsStore settings(storage.settingsFile());
    const auto settingsProtection =
        vusiif::zto::storage::SettingsStore::guardStorageMigration(settings, storage);
    if (!settingsProtection.isEmpty())
        std::fprintf(stderr, "ZTOrbit settings: %s\n", qPrintable(settingsProtection));
    const auto migration = vusiif::zto::storage::SettingsStore::migrateLegacy(
        settings, storage.metadataDirectory(), storage.userKey());
    for (const auto &warning : migration.warnings)
        std::fprintf(stderr, "ZTOrbit settings migration: %s\n", qPrintable(warning));
    if (!migration.error.isEmpty())
        std::fprintf(stderr, "ZTOrbit settings migration: %s\n", qPrintable(migration.error));

    // Select the renderer before creating any Quick window, including errors.
    // The command-line option overrides the saved preference for this run only.
    if (app.arguments().contains("--disable-gpu-acceleration") ||
        settings.value("rendering/disableGpuAcceleration", true).toBool())
        QQuickWindow::setSceneGraphBackend("software");
    QQuickStyle::setStyle("Basic");
#ifdef Q_OS_MACOS
    app.setFont(QFont("PingFang SC", 10));
#else
    app.setFont(QFont("Microsoft YaHei UI", 10));
#endif
    // Blue Z until the controller knows the service state (red = offline).
    app.setWindowIcon(QIcon(QStringLiteral(":/qt/qml/ZTOrbit/assets/logo-online.png")));
    vusiif::zto::platform::SingleInstance instance;
    const auto acquired = instance.acquire();
    if (acquired == vusiif::zto::platform::SingleInstance::Result::Activated) return 0;
    if (acquired == vusiif::zto::platform::SingleInstance::Result::Failed) {
        QQmlApplicationEngine errorEngine;
        errorEngine.setInitialProperties({{"message", instance.error()}});
        errorEngine.load(QUrl("qrc:/qt/qml/ZTOrbit/qml/StartupError.qml"));
        if (!errorEngine.rootObjects().isEmpty()) app.exec();
        return 1;
    }
    vusiif::zto::util::log::initialize();
    vusiif::zto::ui::AppController controller(nullptr, {}, nullptr, storage);
    QQmlApplicationEngine engine;
    engine.setInitialProperties({{"backend", QVariant::fromValue(&controller)}});
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl("qrc:/qt/qml/ZTOrbit/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!window) return 1;
    controller.setWindow(window);
    app.setQuitOnLastWindowClosed(false);
    QObject::connect(&controller, &vusiif::zto::ui::AppController::exitFinished, &app, &QCoreApplication::quit, Qt::QueuedConnection);
    instance.setActivation([&] { controller.restoreWindow(); });
    const int result = app.exec();
    // Runtime libraries may remain mapped until process exit. A hidden copy of
    // this executable waits for the host to exit, then reclaims the session
    // through its registered index token (never an arbitrary path).
    if (storage.isValid() && cacheError.isEmpty() && !cacheContext.token.isEmpty()) {
        QStringList cleanupArguments{"--cleanup-plugin-cache",
            "--cache-run", cacheContext.runId, "--cache-user", cacheContext.userKey,
            "--cache-token", cacheContext.token,
            "--cache-pid", QString::number(QCoreApplication::applicationPid())};
        const auto parentStart = vusiif::zto::package::processStartIdentity(QCoreApplication::applicationPid());
        if (!parentStart.isEmpty()) cleanupArguments << "--cache-parent-start" << parentStart;
        cleanupArguments << "--cache-wait";
        QProcess::startDetached(QCoreApplication::applicationFilePath(), cleanupArguments);
    }
    return result;
}
