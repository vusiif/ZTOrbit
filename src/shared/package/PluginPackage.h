#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QLockFile>
#include <memory>
namespace vusiif::zto::package {
// Explicit extract-cache ownership. The client supplies its active root and
// stable user/run identifiers; the archived server and packaging tools select
// the legacy process-private temporary strategy.
struct CacheContext {
    QString root;       // Absolute session root (may contain load-*/runtime-*).
    QString userKey;
    QString runId;
    QString installKey;
    // Filled by prepareCacheSession() from the ownership record. Cleanup entry
    // points must present this token instead of an arbitrary root path.
    QString token;
    bool legacy = false;
    static CacheContext temporary();
};

struct PackageResult {
    QJsonObject info;
    QString error;
    QByteArray header, encrypted; // No ciphertext is buffered by inspect (including v1).
    QString path;
    // True when the package signature/authentication succeeded, independent of
    // later host/API compatibility checks. Only this flag may make the declared
    // plugin ID trustworthy for data ownership decisions.
    bool authenticated = false;
    bool ok() const { return error.isEmpty(); }
};
PackageResult inspect(const QString &path);
// v2/v3 inspect already authenticates every block. Legacy v1 must also be
// authenticated before its declarations enter an executable dependency plan.
QString authenticate(const PackageResult &package);
QString validate(const QJsonObject &, const QString &host, const QString &api, const QString &iid);
QString write(const QString &library, const QString &output, QJsonObject info);
QString writeBundle(const QJsonObject &manifest, const QString &output, QJsonObject info);
QString validBundle(const QJsonObject &info);
QString safeRelativePath(const QString &path);
QString extractLicenses(const PackageResult &, const QString &destination);
// Creates the session directory, ownership record, session lock and index entry.
// The index entry is mandatory: a failure to register returns an error and the
// caller must not extract into the session. The ownership token is stored in
// context.token for the later cleanup entry point.
QString prepareCacheSession(CacheContext &context);
// Reclaims sessions of this user whose owning process has exited and whose
// ownership record/index entry validate. Live or unknown owners are skipped.
void cleanStaleCache(const CacheContext &context);
// Hidden entry point for normal-exit cleanup after the host process has exited.
// It accepts only the user key/run ID/token identity and resolves the session
// root from the current user's cache index; an unregistered session is refused.
int runCacheCleanupHelper(const QStringList &arguments);
// Stable creation identity of a process. Combined with its PID it detects PID
// reuse instead of trusting a recycled process ID.
QString processStartIdentity(qint64 pid);
// Registers/removes a worker process owned by this cache session. Worker
// processes outlive a crashed host, so stale-session cleanup must verify them
// before removing the session directory.
QString registerCacheWorker(const QString &cacheDirectory, qint64 pid);
void unregisterCacheWorker(const QString &cacheDirectory, qint64 pid);
QString extract(const PackageResult &, const CacheContext &, std::unique_ptr<QTemporaryDir> &directory,
                QString &library, std::unique_ptr<QLockFile> &lock);
// Legacy strategy retained for the archived server and packaging tools.
QString extract(const PackageResult &, std::unique_ptr<QTemporaryDir> &directory, QString &library,
                std::unique_ptr<QLockFile> &lock);
QJsonObject buildInfo();
void cleanStaleCache();
void discardCache(std::unique_ptr<QTemporaryDir> &directory);
QString createCache(const QString &kind, const CacheContext &, std::unique_ptr<QTemporaryDir> &,
                    std::unique_ptr<QLockFile> &);
QString createCache(const QString &kind, std::unique_ptr<QTemporaryDir> &, std::unique_ptr<QLockFile> &);
}
