#include "UpdateStorage.h"

#include "UpdaterEngine.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QUuid>

namespace vusiif::zto::update {
namespace {

QString cleanVersion(const QString &version)
{
    static const QRegularExpression pattern(QStringLiteral("[^0-9A-Za-z.-]"));
    auto result = version;
    result.replace(pattern, QStringLiteral("_"));
    return result.isEmpty() ? QStringLiteral("unknown") : result;
}

QString uniqueId(const QString &version)
{
    return QStringLiteral("%1-%2-%3")
        .arg(cleanVersion(version),
             QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")),
             QUuid::createUuid().toString(QUuid::Id128).left(8));
}

QStringList splitLines(const QString &text)
{
    return text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

} // namespace

UpdatePaths UpdatePaths::forStorageRoot(const QString &storageRoot)
{
    UpdatePaths paths;
    paths.root = QDir(storageRoot).filePath(QStringLiteral("updates"));
    paths.downloads = QDir(paths.root).filePath(QStringLiteral("download"));
    paths.staging = QDir(paths.root).filePath(QStringLiteral("staging"));
    paths.transactions = QDir(paths.root).filePath(QStringLiteral("transaction"));
    paths.runtime = QDir(paths.root).filePath(QStringLiteral("runtime"));
    paths.plans = QDir(paths.root).filePath(QStringLiteral("plan"));
    paths.logs = QDir(paths.root).filePath(QStringLiteral("log"));
    paths.logFile = QDir(paths.logs).filePath(QStringLiteral("updater.log"));
    paths.resultFile = QDir(paths.root).filePath(QStringLiteral("last-result.txt"));
    return paths;
}

bool UpdatePaths::prepare(QString &error) const
{
    for (const auto &directory : {root, downloads, staging, transactions, runtime, plans, logs}) {
        if (!QDir().mkpath(directory)) {
            error = QStringLiteral("无法创建更新目录：%1").arg(QDir::toNativeSeparators(directory));
            return false;
        }
    }
    return true;
}

UpdateResultRecord UpdateResultRecord::take(const QString &path)
{
    UpdateResultRecord record;
    QFile file(path);
    if (!file.exists()) return record;
    if (!file.open(QIODevice::ReadOnly)) return record;
    const auto text = QString::fromUtf8(file.readAll());
    file.close();
    QFile::remove(path);
    record.present = true;
    for (const auto &line : splitLines(text)) {
        const auto equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0) continue;
        const auto key = line.left(equals);
        const auto value = line.mid(equals + 1);
        if (key == QLatin1String("result")) record.ok = value == QLatin1String("ok");
        else if (key == QLatin1String("step")) record.step = value;
        else if (key == QLatin1String("message")) record.message = value;
        else if (key == QLatin1String("version")) record.version = value;
        else if (key == QLatin1String("restart")) record.restart = value;
        else if (key == QLatin1String("time")) record.time = value;
    }
    return record;
}

UpdateStorage::UpdateStorage(const UpdatePaths &paths) : m_paths(paths) {}

QStringList UpdateStorage::recoverInterrupted(QString &error)
{
    error.clear();
    QStringList notes;
    const QDir transactions(m_paths.transactions);
    bool helperRunning = false;
    for (const auto &entry : transactions.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const auto directory = transactions.filePath(entry);
        const auto identityFile = QDir(directory).filePath(QStringLiteral("helper.pid"));
        QFile identity(identityFile);
        if (identity.open(QIODevice::ReadOnly)) {
            const auto parts = QString::fromUtf8(identity.readAll()).trimmed().split(QLatin1Char('|'));
            identity.close();
            if (parts.size() == 2) {
                bool pidOk = false;
                bool creationOk = false;
                const auto pid = parts[0].toULongLong(&pidOk);
                const auto creation = parts[1].toULongLong(&creationOk);
                if (pidOk && creationOk && updater::processIdentityAlive(pid, creation)) {
                    helperRunning = true;
                    notes.append(QStringLiteral("更新程序仍在运行，暂不处理事务目录：%1")
                                     .arg(QDir::toNativeSeparators(directory)));
                    continue;
                }
            }
        }
        std::string recoverError;
        if (!updater::recoverTransaction(directory.toStdString(), recoverError)) {
            notes.append(QStringLiteral("无法恢复上次更新事务：%1")
                             .arg(QString::fromStdString(recoverError)));
            continue;
        }
        notes.append(QStringLiteral("已恢复上次未完成的更新事务。"));
    }
    if (helperRunning) {
        // The helper owns the staging tree until it exits; leave everything.
        return notes;
    }
    for (const auto &directory : {m_paths.staging, m_paths.downloads, m_paths.plans}) {
        const QDir dir(directory);
        for (const auto &entry : dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
            const auto path = dir.filePath(entry);
            std::string removeError;
            if (!updater::removeTransactionDirectory(path.toStdString(), removeError)) {
                notes.append(QStringLiteral("无法清理更新临时文件：%1")
                                 .arg(QString::fromStdString(removeError)));
            }
        }
    }
    // Runtime copies of the helper: only files, and only below our own folder.
    const QDir runtime(m_paths.runtime);
    for (const auto &entry : runtime.entryList(QDir::Files | QDir::Hidden)) {
        QFile::remove(runtime.filePath(entry));
    }
    return notes;
}

QString UpdateStorage::createStagingDirectory(const QString &version, QString &error)
{
    const auto directory = QDir(m_paths.staging).filePath(uniqueId(version));
    if (!QDir().mkpath(directory)) {
        error = QStringLiteral("无法创建更新暂存目录");
        return {};
    }
    return directory;
}

QString UpdateStorage::downloadFilePath(const QString &version, QString &error) const
{
    if (!QDir().mkpath(m_paths.downloads)) {
        error = QStringLiteral("无法创建下载目录");
        return {};
    }
    return QDir(m_paths.downloads).filePath(uniqueId(version) + QStringLiteral(".zip"));
}

QString UpdateStorage::writePlan(const updater::Plan &plan, QString &error) const
{
    std::string text;
    std::string planError;
    if (!updater::serializePlan(plan, text, planError)) {
        error = QString::fromStdString(planError);
        return {};
    }
    const auto id = QFileInfo(QString::fromStdString(plan.transactionDir)).fileName();
    const auto path = QDir(m_paths.plans).filePath(id + QStringLiteral(".plan"));
    std::string writeError;
    if (!updater::writeTextFileAtomically(path.toStdString(), text, writeError)) {
        error = QString::fromStdString(writeError);
        return {};
    }
    return path;
}

QString UpdateStorage::helperExecutable(const QString &executableDirectory) const
{
    return QDir(executableDirectory).filePath(QStringLiteral("z2_updater.exe"));
}

bool UpdateStorage::launchHelper(const QString &helperPath, const QString &planPath,
                                 const QString &workingDirectory, bool noRestart, qint64 *pid,
                                 QString &error) const
{
    error.clear();
    if (!QFileInfo::exists(helperPath)) {
        error = QStringLiteral("找不到更新程序 z2_updater.exe");
        return false;
    }
    QStringList arguments{QStringLiteral("--plan"), planPath};
    if (noRestart) arguments.append(QStringLiteral("--no-restart"));
    qint64 processId = 0;
    if (!QProcess::startDetached(helperPath, arguments, workingDirectory, &processId)) {
        error = QStringLiteral("无法启动更新程序");
        return false;
    }
    if (pid) *pid = processId;
    return true;
}

bool UpdateStorage::removeDirectoryTree(const QString &path, QString &error)
{
    std::string removeError;
    if (!updater::removeTransactionDirectory(path.toStdString(), removeError)) {
        error = QString::fromStdString(removeError);
        return false;
    }
    return true;
}

} // namespace vusiif::zto::update
