#include "StorageMaintenanceScheduler.h"

#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

namespace vusiif::zto::storage {
namespace {
bool isCachePath(const QString &path)
{
    auto normalized = path;
    normalized.replace('\\', '/');
    return normalized == QLatin1String("cache") || normalized.startsWith(QLatin1String("cache/"));
}

void merge(CleanupReport &target, const CleanupReport &source)
{
    target.releasedBytes += source.releasedBytes;
    target.released.append(source.released);
    target.skipped.append(source.skipped);
    target.failed.append(source.failed);
    if (!source.error.isEmpty()) {
        if (target.error.isEmpty()) target.error = source.error;
        else target.error += QLatin1Char('\n') + source.error;
    }
}
} // namespace

StorageMaintenanceScheduler::StorageMaintenanceScheduler(const StorageContext &context, QObject *parent)
    : QObject(parent), m_context(context)
{
    m_startupTimer.setSingleShot(true);
    m_debounceTimer.setSingleShot(true);
    m_dailyTimer.setInterval(m_dailyIntervalMs);
    connect(&m_startupTimer, &QTimer::timeout, this, [this] { run(true, true); });
    connect(&m_dailyTimer, &QTimer::timeout, this, [this] { run(true, true); });
    connect(&m_debounceTimer, &QTimer::timeout, this, [this] {
        const bool logs = m_pendingLogs;
        const bool caches = m_pendingCaches;
        m_pendingLogs = false;
        m_pendingCaches = false;
        run(logs, caches);
    });
    connect(&m_watcher, &QFutureWatcher<CleanupReport>::finished, this,
            &StorageMaintenanceScheduler::handleFinished);
}

StorageMaintenanceScheduler::~StorageMaintenanceScheduler()
{
    stop();
    disconnect(&m_watcher, nullptr, this, nullptr);
    if (m_busy) m_watcher.waitForFinished();
}

void StorageMaintenanceScheduler::setStartupDelayMs(int milliseconds)
{
    m_startupDelayMs = qMax(0, milliseconds);
}

void StorageMaintenanceScheduler::setDailyIntervalMs(int milliseconds)
{
    m_dailyIntervalMs = qMax(1, milliseconds);
    m_dailyTimer.setInterval(m_dailyIntervalMs);
}

void StorageMaintenanceScheduler::setDebounceMs(int milliseconds)
{
    m_debounceMs = qMax(0, milliseconds);
}

void StorageMaintenanceScheduler::setCleanupTask(CleanupTask task) { m_task = std::move(task); }

void StorageMaintenanceScheduler::start()
{
    if (m_started) return;
    m_started = true;
    // Do not block window creation or plugin discovery: the first pass runs
    // after the event loop has handled the startup work.
    m_startupTimer.start(m_startupDelayMs);
    m_dailyTimer.start();
}

void StorageMaintenanceScheduler::stop()
{
    m_started = false;
    m_startupTimer.stop();
    m_dailyTimer.stop();
    m_debounceTimer.stop();
    m_pendingLogs = false;
    m_pendingCaches = false;
}

void StorageMaintenanceScheduler::notifyLogRotation()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this] { notifyLogRotation(); }, Qt::QueuedConnection);
        return;
    }
    if (!m_started) return;
    m_pendingLogs = true;
    m_debounceTimer.start(m_debounceMs);
}

void StorageMaintenanceScheduler::notifyCacheWrite(const QString &relativePath)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, relativePath] { notifyCacheWrite(relativePath); },
                                  Qt::QueuedConnection);
        return;
    }
    if (!m_started || !isCachePath(relativePath)) return;
    m_pendingCaches = true;
    m_debounceTimer.start(m_debounceMs);
}

void StorageMaintenanceScheduler::run(bool logs, bool caches)
{
    if (!logs && !caches) return;
    if (m_busy) {
        m_pendingLogs |= logs;
        m_pendingCaches |= caches;
        return;
    }
    m_busy = true;
    const auto context = m_context;
    const auto task = m_task;
    m_watcher.setFuture(QtConcurrent::run([context, task, logs, caches] {
        if (task) return task(logs, caches);
        StorageMaintenance maintenance(context);
        CleanupReport report;
        if (logs) merge(report, maintenance.cleanupLogs());
        if (caches) merge(report, maintenance.cleanupCaches());
        return report;
    }));
}

void StorageMaintenanceScheduler::handleFinished()
{
    m_busy = false;
    m_lastReport = m_watcher.result();
    m_lastRun = QDateTime::currentDateTimeUtc();
    if (m_lastReport.ok()) m_lastError.clear();
    else m_lastError = m_lastReport.error;
    emit cleanupFinished(m_lastReport);
    if (!m_lastError.isEmpty()) emit cleanupFailed(m_lastError);
    if (m_pendingLogs || m_pendingCaches) {
        const bool logs = m_pendingLogs;
        const bool caches = m_pendingCaches;
        m_pendingLogs = false;
        m_pendingCaches = false;
        run(logs, caches);
    }
}

} // namespace vusiif::zto::storage
