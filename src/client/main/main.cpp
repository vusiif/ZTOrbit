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
        QCoreApplication::setOrganizationName("ZeroTierGUI");
        QCoreApplication::setApplicationName("ZTGUI");
        return vusiif::ztgui::package::runCacheCleanupHelper(cleanup.arguments());
    }

    // Qt.labs.platform uses the Widgets backend for native tray menus on Windows.
    QApplication app(argc, argv);
    app.setApplicationName("ZTGUI");
    app.setApplicationVersion(ZEROTIER_GUI_VERSION);
    app.setOrganizationName("ZeroTierGUI");

    // Select the storage root before reading any host setting. The shared
    // application directory is used while it is actually writable; otherwise
    // this user gets a personal profile and the choice is recorded.
    auto storage = vusiif::ztgui::storage::StorageContext::resolve();
    QString storageError;
    if (storage.isValid()) storage.prepare(storageError);
    else storageError = storage.error();
    if (!storageError.isEmpty())
        std::fprintf(stderr, "ZTGUI storage: %s\n", qPrintable(storageError));
    vusiif::ztgui::util::log::setDirectory(storage.logsDirectory());
    vusiif::ztgui::package::CacheContext cacheContext;
    cacheContext.root = storage.pluginPackageCacheRoot();
    cacheContext.userKey = storage.userKeyValue();
    cacheContext.runId = storage.runId();
    cacheContext.installKey = storage.installKeyValue();
    cacheContext.legacy = false;
    const auto cacheError = vusiif::ztgui::package::prepareCacheSession(cacheContext);
    if (!cacheError.isEmpty())
        std::fprintf(stderr, "ZTGUI cache session: %s\n", qPrintable(cacheError));
    vusiif::ztgui::package::cleanStaleCache(cacheContext);
    vusiif::ztgui::storage::SettingsStore settings(storage.settingsFile());
    const auto settingsProtection =
        vusiif::ztgui::storage::SettingsStore::guardStorageMigration(settings, storage);
    if (!settingsProtection.isEmpty())
        std::fprintf(stderr, "ZTGUI settings: %s\n", qPrintable(settingsProtection));
    const auto migration = vusiif::ztgui::storage::SettingsStore::migrateLegacy(
        settings, storage.metadataDirectory(), storage.userKey());
    for (const auto &warning : migration.warnings)
        std::fprintf(stderr, "ZTGUI settings migration: %s\n", qPrintable(warning));
    if (!migration.error.isEmpty())
        std::fprintf(stderr, "ZTGUI settings migration: %s\n", qPrintable(migration.error));

    // Select the renderer before creating any Quick window, including errors.
    // The command-line option overrides the saved preference for this run only.
    if (app.arguments().contains("--disable-gpu-acceleration") ||
        settings.value("rendering/disableGpuAcceleration", true).toBool())
        QQuickWindow::setSceneGraphBackend("software");
    QQuickStyle::setStyle("Basic");
#ifdef Q_OS_MACOS
    app.setFont(QFont("PingFang SC", 10));
    app.setWindowIcon(QIcon(":/assets/app-icon.png"));
#else
    app.setFont(QFont("Microsoft YaHei UI", 10));
    app.setWindowIcon(QIcon(":/assets/app-icon.ico"));
#endif
    vusiif::ztgui::platform::SingleInstance instance;
    const auto acquired = instance.acquire();
    if (acquired == vusiif::ztgui::platform::SingleInstance::Result::Activated) return 0;
    if (acquired == vusiif::ztgui::platform::SingleInstance::Result::Failed) {
        QQmlApplicationEngine errorEngine;
        errorEngine.setInitialProperties({{"message", instance.error()}});
        errorEngine.load(QUrl("qrc:/qt/qml/ZTGUI/qml/StartupError.qml"));
        if (!errorEngine.rootObjects().isEmpty()) app.exec();
        return 1;
    }
    vusiif::ztgui::util::log::initialize();
    vusiif::ztgui::ui::AppController controller(nullptr, {}, nullptr, storage);
    QQmlApplicationEngine engine;
    engine.setInitialProperties({{"backend", QVariant::fromValue(&controller)}});
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl("qrc:/qt/qml/ZTGUI/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!window) return 1;
    controller.setWindow(window);
    app.setQuitOnLastWindowClosed(false);
    QObject::connect(&controller, &vusiif::ztgui::ui::AppController::exitFinished, &app, &QCoreApplication::quit, Qt::QueuedConnection);
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
        const auto parentStart = vusiif::ztgui::package::processStartIdentity(QCoreApplication::applicationPid());
        if (!parentStart.isEmpty()) cleanupArguments << "--cache-parent-start" << parentStart;
        cleanupArguments << "--cache-wait";
        QProcess::startDetached(QCoreApplication::applicationFilePath(), cleanupArguments);
    }
    return result;
}
