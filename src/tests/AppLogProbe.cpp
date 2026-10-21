#include "AppLog.h"
#include <QCoreApplication>
#include <QDebug>
#include <cstdio>
#include <thread>
#include <vector>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    vusiif::ztgui::util::log::initialize();
    vusiif::ztgui::util::log::initialize(); // Reinitializing must not allocate a second log.
    if (app.arguments().contains(QStringLiteral("--rotation"))) {
        // Trigger one real 10 MiB rotation and prove the callback runs after
        // the log mutex has been released.
        int rotations = 0;
        vusiif::ztgui::util::log::setRotationCallback([&rotations] { ++rotations; });
        const QString oversized(10 * 1024 * 1024 + 4096, QLatin1Char('x'));
        vusiif::ztgui::util::log::write(oversized);
        vusiif::ztgui::util::log::write(QStringLiteral("after rotation"));
        std::fprintf(stdout, "%d\n", rotations);
        return rotations == 1 ? 0 : 1;
    }
    vusiif::ztgui::util::log::write(QStringLiteral("日志测试：中文"));
    qWarning("captured warning");
    std::vector<std::thread> writers;
    for (int worker = 0; worker < 4; ++worker) {
        writers.emplace_back([worker] {
            for (int entry = 0; entry < 10; ++entry)
                vusiif::ztgui::util::log::write(QStringLiteral("worker=%1 entry=%2").arg(worker).arg(entry));
        });
    }
    for (auto &writer : writers) writer.join();
    const QString path = vusiif::ztgui::util::log::path();
    std::fprintf(stdout, "%s\n", path.toUtf8().constData());
    return path.isEmpty() ? 1 : 0;
}
