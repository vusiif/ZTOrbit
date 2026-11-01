// Test helper: plays the loading side of the delete-recovery race.
//
// The probe acquires the data-root coordination lock, signals through a marker
// file, waits so the recovering process reaches the same lock, then registers a
// plugin lease while it still holds the lock and releases it. Recovery may only
// decide about the package inside that same lock interval, so it must see the
// lease and keep the installed package.
#include "storage/CoordinationLock.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() < 5) return 2;
    const auto metadataRoot = arguments.at(1);
    const auto markerPath = arguments.at(2);
    const auto leaseFile = arguments.at(3);
    const int holdMilliseconds = arguments.at(4).toInt();

    vusiif::zto::storage::CoordinationLock lock(metadataRoot);
    QString error;
    if (!lock.acquire(error, 10000)) return 3;
    if (!QDir().mkpath(QFileInfo(markerPath).absolutePath())) return 4;
    {
        QFile marker(markerPath);
        if (!marker.open(QIODevice::WriteOnly)) return 5;
        marker.write("held");
    }
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < holdMilliseconds) QThread::msleep(10);
    if (!QDir().mkpath(QFileInfo(leaseFile).absolutePath())) return 6;
    {
        QFile lease(leaseFile);
        if (!lease.open(QIODevice::WriteOnly)) return 7;
        const QJsonObject record{{"format", 1},
                                 {"pluginId", QStringLiteral("test-dynamic")},
                                 {"runId", QStringLiteral("run-other")}};
        const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Indented);
        if (lease.write(bytes) != bytes.size()) return 8;
    }
    lock.release();
    return 0;
}
