#include "AppLog.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <cstdio>
#include <cstdlib>

namespace {
QMutex mutex;
void messageHandler(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    const char *level = type == QtDebugMsg ? "DEBUG" : type == QtInfoMsg ? "INFO"
        : type == QtWarningMsg ? "WARN" : type == QtCriticalMsg ? "ERROR" : "FATAL";
    AppLog::write(QString::fromLatin1(level) + " " + message);
    if (type == QtFatalMsg) std::abort();
}
}

QString AppLog::path()
{
#ifdef Q_OS_MACOS
    QDir directory(QDir::homePath() + "/Library/Logs/ZTGUI");
    QDir().mkpath(directory.absolutePath());
    return directory.filePath("zerotier-gui.log");
#else
    return QDir(QCoreApplication::applicationDirPath()).filePath("zerotier-gui.log");
#endif
}

void AppLog::write(const QString &text)
{
    QMutexLocker lock(&mutex);
    QFile file(path());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        std::fprintf(stderr, "Cannot open application log: %s\n", qPrintable(file.errorString()));
        return;
    }
    const QString entry = QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
        + " " + text.trimmed() + '\n';
    file.write(entry.toUtf8());
}

void AppLog::initialize()
{
    qInstallMessageHandler(messageHandler);
    write(QStringLiteral("应用启动"));
}
