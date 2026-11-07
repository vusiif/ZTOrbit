#pragma once

#include "UpdaterPlan.h"

#include <QString>
#include <QStringList>

namespace vusiif::zto::update {

// Directory layout below the host storage root (StorageContext::activeRoot()).
// Nothing is ever written inside the live installation tree except through the
// verified transaction plan.
struct UpdatePaths {
    QString root;         // <storage>/updates
    QString downloads;    // <root>/download
    QString staging;      // <root>/staging
    QString transactions; // <root>/transaction
    QString runtime;      // <root>/runtime
    QString plans;        // <root>/plan
    QString logs;         // <root>/log
    QString logFile;      // <root>/log/updater.log
    QString resultFile;   // <root>/last-result.txt

    static UpdatePaths forStorageRoot(const QString &storageRoot);
    bool prepare(QString &error) const;
};

struct UpdateResultRecord {
    bool present = false;
    bool ok = false;
    QString step;
    QString message;
    QString version;
    QString restart;
    QString time;

    // Reads and removes the helper's result file. A missing file is not an
    // error; the record is simply not present.
    static UpdateResultRecord take(const QString &path);
};

class UpdateStorage {
public:
    explicit UpdateStorage(const UpdatePaths &paths);
    const UpdatePaths &paths() const { return m_paths; }

    // Handles a transaction left behind by a previous process: a live helper is
    // never disturbed; otherwise the backup is restored (when the transaction
    // did not commit) and the directory is removed. Also removes stale staging,
    // download, plan and runtime leftovers. Returns a list of human-readable
    // notes; a fatal problem is returned in `error`.
    QStringList recoverInterrupted(QString &error);

    // Fresh empty staging directory for `version`; empty on failure.
    QString createStagingDirectory(const QString &version, QString &error);
    QString downloadFilePath(const QString &version, QString &error) const;

    // Writes the plan atomically and returns its path.
    QString writePlan(const updater::Plan &plan, QString &error) const;

    QString helperExecutable(const QString &executableDirectory) const;

    // Starts z2_updater.exe detached. Returns false without starting anything
    // when the executable is missing.
    bool launchHelper(const QString &helperPath, const QString &planPath,
                      const QString &workingDirectory, bool noRestart, qint64 *pid,
                      QString &error) const;

    static bool removeDirectoryTree(const QString &path, QString &error);

private:
    UpdatePaths m_paths;
};

} // namespace vusiif::zto::update
