#include "AppLog.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
QMutex mutex;
QFile logFile;
bool openAttempted = false;
bool initialized = false;

// Called with mutex held. NewOnly reserves the filename across processes too.
bool openLog()
{
    if (openAttempted) return logFile.isOpen();
    openAttempted = true;
    const QDir applicationDirectory(QDir::currentPath());
    const QDir directory(applicationDirectory.filePath("logs"));
    if (!QDir().mkpath(directory.absolutePath())) {
        std::fprintf(stderr, "Cannot create application log directory: %s\n", qPrintable(directory.absolutePath()));
        return false;
    }
    const QString date = QDateTime::currentDateTime().toString("yyMMdd");
    const QRegularExpression pattern("^" + date + "-([1-9][0-9]*)\\.log$");
    quint64 sequence = 0;
    for (const QString &name : directory.entryList(QDir::Files | QDir::Hidden | QDir::System)) {
        const auto match = pattern.match(name);
        if (!match.hasMatch()) continue;
        bool ok = false;
        const quint64 number = match.captured(1).toULongLong(&ok);
        if (ok && number > sequence) sequence = number;
    }
    while (sequence < std::numeric_limits<quint64>::max()) {
        logFile.setFileName(directory.filePath(date + "-" + QString::number(++sequence) + ".log"));
        if (logFile.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Text)) return true;
        if (!QFile::exists(logFile.fileName())) {
            std::fprintf(stderr, "Cannot open application log: %s\n", qPrintable(logFile.errorString()));
            return false;
        }
    }
    std::fprintf(stderr, "Application log sequence exhausted\n");
    return false;
}

QString appendEntry(const QString &text)
{
    if (!openLog()) return QStringLiteral("Cannot open application log");
    const QByteArray entry = (QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
        + " " + text.trimmed() + '\n').toUtf8();
    if (logFile.write(entry) != entry.size() || !logFile.flush()) {
        std::fprintf(stderr, "Cannot write application log: %s\n", qPrintable(logFile.errorString()));
        return logFile.errorString();
    }
    return {};
}

void messageHandler(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    const char *level = type == QtDebugMsg ? "DEBUG" : type == QtInfoMsg ? "INFO"
        : type == QtWarningMsg ? "WARN" : type == QtCriticalMsg ? "ERROR" : "FATAL";
    vusiif::zto::util::log::write(QString::fromLatin1(level) + " " + message);
    if (type == QtFatalMsg) std::abort();
}
}

QString vusiif::zto::util::log::path()
{
    QMutexLocker lock(&mutex);
    return openLog() ? logFile.fileName() : QString();
}

void vusiif::zto::util::log::write(const QString &text)
{
    QMutexLocker lock(&mutex);
    appendEntry(text);
}

QString vusiif::zto::util::log::writeChecked(const QString &text)
{
    QMutexLocker lock(&mutex);
    return appendEntry(text);
}

void vusiif::zto::util::log::initialize()
{
    QMutexLocker lock(&mutex);
    if (initialized) return;
    initialized = true;
    openLog();
    qInstallMessageHandler(messageHandler);
    appendEntry(QStringLiteral("服务启动 ztohub v%1").arg(QStringLiteral(ZTO_VERSION)));
}
