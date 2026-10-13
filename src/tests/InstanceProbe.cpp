#include "SingleInstance.h"
#include <QGuiApplication>
#include <QWindow>
#include <QThread>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <QTimer>
#include <iostream>
int main(int argc, char **argv) {
    QGuiApplication app(argc,argv);
    vusiif::ztgui::platform::SingleInstance instance(nullptr,"-tests");
    const auto result = instance.acquire();
    if (result == vusiif::ztgui::platform::SingleInstance::Result::Activated) { std::cout << "secondary" << std::endl; return 0; }
    if (result != vusiif::ztgui::platform::SingleInstance::Result::Primary) return 2;
    std::cout << "primary" << std::endl;
    QWindow window;
    const bool native = qEnvironmentVariableIsSet("ZTGUI_INSTANCE_NATIVE");
    if (native) { window.setTitle("ZTGUI instance verification"); window.showMinimized(); }
    if (qEnvironmentVariableIsSet("ZTGUI_INSTANCE_START_DELAY")) QThread::msleep(500);
    instance.setActivation([&] {
        if (native) {
            vusiif::ztgui::platform::SingleInstance::activateWindow(window);
#ifdef Q_OS_WIN
            QTimer::singleShot(100,&app,[&] {
                std::cout << (GetForegroundWindow()==reinterpret_cast<HWND>(window.winId()) && window.windowState() != Qt::WindowMinimized ? "foreground" : "focus-failed") << std::endl;
            });
#endif
        }
        std::cout << "activated" << std::endl;
    });
    QTimer::singleShot(12000,&app,&QCoreApplication::quit);
    return app.exec();
}
