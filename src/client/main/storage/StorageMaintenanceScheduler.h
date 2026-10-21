#pragma once

#include "StorageMaintenance.h"

#include <QDateTime>
#include <QFutureWatcher>
#include <QObject>
#include <QTimer>

#include <functional>

namespace vusiif::ztgui::storage {

// Single scheduler for automatic retention work. Startup, the daily timer, log
// rotation and business-cache writes all funnel through this class, and only
// one cleanup task runs at a time. The GUI thread only starts the task and
// handles its result; directory walks and deletions run on a worker thread.
class StorageMaintenanceScheduler final : public QObject {
    Q_OBJECT
public:
    using CleanupTask = std::function<CleanupReport(bool logs, bool caches)>;

    explicit StorageMaintenanceScheduler(const StorageContext &context, QObject *parent = nullptr);
    ~StorageMaintenanceScheduler() override;

    // Intervals are injectable so tests do not wait for production delays.
    void setStartupDelayMs(int milliseconds);
    void setDailyIntervalMs(int milliseconds);
    void setDebounceMs(int milliseconds);
    // Tests replace the filesystem cleanup with a deterministic probe. The
    // default task reuses StorageMaintenance exactly like the manual buttons.
    void setCleanupTask(CleanupTask task);

    void start();
    void stop();
    bool started() const { return m_started; }
    bool busy() const { return m_busy; }
    QString lastError() const { return m_lastError; }
    CleanupReport lastReport() const { return m_lastReport; }
    QDateTime lastRun() const { return m_lastRun; }

    // Safe to call from any thread (plugin log writes and file commits).
    void notifyLogRotation();
    void notifyCacheWrite(const QString &relativePath);

signals:
    void cleanupFinished(const vusiif::ztgui::storage::CleanupReport &report);
    void cleanupFailed(const QString &error);

private:
    void run(bool logs, bool caches);
    void handleFinished();

    StorageContext m_context;
    CleanupTask m_task;
    QTimer m_startupTimer;
    QTimer m_dailyTimer;
    QTimer m_debounceTimer;
    QFutureWatcher<CleanupReport> m_watcher;
    int m_startupDelayMs = 5000;
    int m_dailyIntervalMs = 24 * 60 * 60 * 1000;
    int m_debounceMs = 5000;
    bool m_started = false;
    bool m_busy = false;
    bool m_pendingLogs = false;
    bool m_pendingCaches = false;
    QString m_lastError;
    CleanupReport m_lastReport;
    QDateTime m_lastRun;
};

} // namespace vusiif::ztgui::storage
