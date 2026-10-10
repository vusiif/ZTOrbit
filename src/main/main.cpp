#include <QApplication>
#include <QIcon>
#include <QSettings>
#include "AppStyle.h"
#include "AppLog.h"
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("ZTGUI");
    app.setApplicationVersion(ZEROTIER_GUI_VERSION);
    app.setOrganizationName("ZeroTierGUI");
    AppLog::initialize();
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
    applyAppStyle(app);
    MainWindow window;
    window.show();
    return app.exec();
}
