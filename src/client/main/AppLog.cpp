#include "AppLog.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
QMutex mutex;
QFile logFile;
bool openAttempted = false;
bool initialized = false;
QString selectedDirectory;
QSet<QString> activeFiles;
std::function<void()> rotationCallback;
constexpr qint64 MaxFileBytes = 10 * 1024 * 1024;

QString normalizedActivePath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

// Called with mutex held. NewOnly reserves the filename across processes too.
bool openLog()
{
    if (openAttempted) return logFile.isOpen();
    openAttempted = true;
    const QDir applicationDirectory(QCoreApplication::applicationDirPath());
    const QDir directory(selectedDirectory.isEmpty() ? applicationDirectory.filePath("logs") : selectedDirectory);
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
        if (logFile.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Text)) {
            activeFiles.insert(normalizedActivePath(logFile.fileName()));
            return true;
        }
        if (!QFile::exists(logFile.fileName())) {
            std::fprintf(stderr, "Cannot open application log: %s\n", qPrintable(logFile.errorString()));
            return false;
        }
    }
    std::fprintf(stderr, "Application log sequence exhausted\n");
    return false;
}

QString appendEntry(const QString &text, bool *rotated = nullptr)
{
    if (rotated) *rotated = false;
    // Rotate at the fixed cap; the next NewOnly open picks a fresh sequence.
    if (logFile.isOpen() && logFile.size() >= MaxFileBytes) {
        activeFiles.remove(normalizedActivePath(logFile.fileName()));
        logFile.close();
        openAttempted = false;
        if (rotated) *rotated = true;
    }
    if (!openLog()) return QStringLiteral("Cannot open application log");
    const QByteArray entry = (QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
        + " " + text.trimmed() + '\n').toUtf8();
    if (logFile.write(entry) != entry.size() || !logFile.flush()) {
        std::fprintf(stderr, "Cannot write application log: %s\n", qPrintable(logFile.errorString()));
        return logFile.errorString();
    }
    return {};
}

// Runs the rotation callback after releasing the mutex so callbacks cannot
// deadlock by logging again.
QString writeEntry(const QString &text)
{
    std::function<void()> callback;
    QString result;
    {
        QMutexLocker lock(&mutex);
        bool rotated = false;
        result = appendEntry(text, &rotated);
        if (rotated) callback = rotationCallback;
    }
    if (callback) callback();
    return result;
}

void messageHandler(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    const char *level = type == QtDebugMsg ? "DEBUG" : type == QtInfoMsg ? "INFO"
        : type == QtWarningMsg ? "WARN" : type == QtCriticalMsg ? "ERROR" : "FATAL";
    vusiif::ztgui::util::log::write(QString::fromLatin1(level) + " " + message);
    if (type == QtFatalMsg) std::abort();
}
}

QString vusiif::ztgui::util::log::path()
{
    QMutexLocker lock(&mutex);
    return openLog() ? logFile.fileName() : QString();
}

void vusiif::ztgui::util::log::setDirectory(const QString &directory)
{
    QMutexLocker lock(&mutex);
    if (openAttempted) return;
    selectedDirectory = directory;
}

QString vusiif::ztgui::util::log::directory()
{
    QMutexLocker lock(&mutex);
    if (!selectedDirectory.isEmpty()) return QDir(selectedDirectory).absolutePath();
    return QDir(QCoreApplication::applicationDirPath()).filePath("logs");
}

void vusiif::ztgui::util::log::write(const QString &text)
{
    writeEntry(text);
}

QString vusiif::ztgui::util::log::writeChecked(const QString &text)
{
    return writeEntry(text);
}

void vusiif::ztgui::util::log::setRotationCallback(std::function<void()> callback)
{
    QMutexLocker lock(&mutex);
    rotationCallback = std::move(callback);
}

void vusiif::ztgui::util::log::initialize()
{
    QMutexLocker lock(&mutex);
    if (initialized) return;
    initialized = true;
    openLog();
    qInstallMessageHandler(messageHandler);
    appendEntry(QStringLiteral("应用启动 ZTGUI v%1").arg(QStringLiteral(ZEROTIER_GUI_VERSION)));
}

void vusiif::ztgui::util::log::registerActiveFile(const QString &path)
{
    QMutexLocker lock(&mutex);
    activeFiles.insert(normalizedActivePath(path));
}

void vusiif::ztgui::util::log::unregisterActiveFile(const QString &path)
{
    QMutexLocker lock(&mutex);
    activeFiles.remove(normalizedActivePath(path));
}

bool vusiif::ztgui::util::log::isActiveFile(const QString &path)
{
    QMutexLocker lock(&mutex);
    return activeFiles.contains(normalizedActivePath(path));
}

qint64 vusiif::ztgui::util::log::maximumFileBytes() { return MaxFileBytes; }
