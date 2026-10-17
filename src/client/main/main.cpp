#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QFont>
#include <QIcon>
#include <QSettings>
#include "AppController.h"
#include "AppLog.h"
#include "SingleInstance.h"

int main(int argc, char *argv[])
{
    // Qt.labs.platform uses the Widgets backend for native tray menus on Windows.
    QApplication app(argc, argv);
    app.setApplicationName("ZTGUI");
    app.setApplicationVersion(ZEROTIER_GUI_VERSION);
    app.setOrganizationName("ZeroTierGUI");
    QSettings settings;
    if (settings.allKeys().isEmpty()) {
        const QSettings previous("ZeroTierGUI", "ZeroTier GUI");
        for (const auto &key : previous.allKeys()) settings.setValue(key, previous.value(key));
    }
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
    vusiif::ztgui::ui::AppController controller;
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
    return app.exec();
}
