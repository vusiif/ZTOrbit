#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QThread>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 4) return 2;
    const QString directory = QString::fromLocal8Bit(argv[1]);
    const QString marker = QString::fromLocal8Bit(argv[2]);
    const int holdMilliseconds = QString::fromLocal8Bit(argv[3]).toInt();
    QLockFile lock(QDir(directory).filePath(QStringLiteral(".import.lock")));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(5000)) return 3;
    QFile file(marker);
    if (!file.open(QIODevice::WriteOnly)) return 4;
    file.write("locked");
    file.close();
    QThread::msleep(static_cast<unsigned long>(qMax(0, holdMilliseconds)));
    return 0;
}
