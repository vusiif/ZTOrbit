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
