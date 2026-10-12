#include <QApplication>
#include <QIcon>
#include <QSettings>
#include "AppStyle.h"
#include "AppLog.h"
#include "MainWindow.h"
#include "SingleInstance.h"
#include <QMessageBox>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("ZTGUI");
    app.setApplicationVersion(ZEROTIER_GUI_VERSION);
    app.setOrganizationName("ZeroTierGUI");
    vusiif::ztgui::platform::SingleInstance instance;
    const auto result = instance.acquire();
    if (result == vusiif::ztgui::platform::SingleInstance::Result::Activated) return 0;
    if (result == vusiif::ztgui::platform::SingleInstance::Result::Failed) {
        QMessageBox::warning(nullptr, QStringLiteral("ZTGUI"), instance.error()); return 1;
    }
    vusiif::ztgui::util::log::initialize();
    QSettings settings;
    if (settings.allKeys().isEmpty()) {
        const QSettings previous("ZeroTierGUI", "ZeroTier GUI");
        for (const auto &key : previous.allKeys()) settings.setValue(key, previous.value(key));
    }
#ifdef Q_OS_MACOS
    app.setWindowIcon(QIcon(QStringLiteral(":/assets/app-icon.png")));
#else
    app.setWindowIcon(QIcon(QStringLiteral(":/assets/app-icon.ico")));
#endif
    vusiif::ztgui::ui::applyAppStyle(app);
    vusiif::ztgui::ui::MainWindow window;
    // Hiding the last window must keep the tray app alive. Explicit exit is
    // emitted only after asynchronous plugin shutdown has finished.
    app.setQuitOnLastWindowClosed(false);
    QObject::connect(&window, &vusiif::ztgui::ui::MainWindow::exitFinished,
                     &app, &QCoreApplication::quit, Qt::QueuedConnection);
    window.show();
    instance.setActivation([&window] {
        if (!window.isClosing()) vusiif::ztgui::platform::SingleInstance::activateWindow(window);
    });
    return app.exec();
}
