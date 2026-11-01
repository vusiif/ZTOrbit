#pragma once
#include <QObject>
#include <QStringList>
#include <QThreadPool>
#include <QFutureWatcher>
#include <QPointer>
#include <QTimer>
#include <zto/PluginLifecycle.h>
#include <memory>
#include <vector>
#include <QVariantList>
#include <QTemporaryDir>
#include <QLockFile>
#include <zto/PluginStop.h>
#include "PluginPackage.h"
#include "ServiceBroker.h"
#include "../storage/StorageContext.h"
#include "../storage/PluginDataStore.h"
#include <functional>
class QPluginLoader;
namespace vusiif::zto::util { class HostServicesImpl; }
namespace vusiif::zto::storage { class SettingsStore; }
namespace vusiif::zto::plugins {
class PluginRegistry;

class PluginLoader final : public QObject {
    Q_OBJECT
public:
    PluginLoader();
    ~PluginLoader();
    // Host settings for persistent enable/keep state. When unset (tests and
    // legacy callers) the default QSettings source is used.
    void setSettings(storage::SettingsStore *settings) { m_settings = settings; }
    void setStorage(const storage::StorageContext &storage) {
        m_storage = storage;
        m_cacheContext = package::CacheContext::temporary();
        if (storage.isValid()) {
            m_cacheContext.root = storage.pluginPackageCacheRoot();
            m_cacheContext.userKey = storage.userKeyValue();
            m_cacheContext.runId = storage.runId();
            m_cacheContext.installKey = storage.installKeyValue();
            m_cacheContext.legacy = false;
        }
    }
    QStringList scan(const QString &directory, PluginRegistry &registry);
    // Accepts a background load; loadFinished reports readiness or failure.
    api::Result loadFile(const QString &path, PluginRegistry &registry, bool enable = true);
    QVariantList catalog() const;
    // True when an installed package exists whose plugin ID cannot be
    // authenticated. Orphan-data classification must then treat stable-ID
    // directories as ambiguous instead of deletable.
    bool hasUncertainPackages() const;
    enum class Removal {
        // Requires an authenticated package identity and deletes the stable-ID
        // data of the active root as well.
        Full,
        // Explicit cleanup for an unauthenticated/corrupt package: removes only
        // the package file and keeps every data and metadata file.
        PackageOnly,
    };
    // Full delete: re-verifies the authenticated package, ID, path bounds and
    // remaining install references, reads the host ownership record, records
    // the authorized operation before moving anything, deletes the stable-ID
    // data of the active root and removes the package under the package change
    // lock. Unauthenticated packages are rejected unless Removal::PackageOnly
    // was explicitly requested.
    api::Result removeFile(const QString &path, const QString &pluginDirectory,
                           const QString &dataRoot = {}, Removal removal = Removal::Full);
    // Describes what a delete would remove without changing any file. The data
    // is only removable when the installed package was authenticated during the
    // scan and its declared ID is valid.
    QVariantMap deletionPlan(const QString &path) const;
    // Continues authorized deletes that were recorded by removeFile(). Must run
    // after the storage root is known and before the plugin scan.
    int recoverPendingDeletions(const QString &pluginDirectory, const QString &dataRoot = {});
    QVariantList pendingDeletions() const { return m_pendingDeletions; }
    QStringList pendingDeletionIssues() const { return m_pendingDeletionIssues; }
    // Display-name claims of the last authenticated scan. They are handed to
    // every consumer so a shared name or an unreadable ownership record keeps
    // legacy data directories instead of letting the first ID claim them.
    const storage::PluginDataStore::InstalledNameClaims &installedNameClaims() const {
        return m_installedNameClaims;
    }
    // Notified for successful plugin file commits below cache/ so the host can
    // debounce business-cache retention without scanning on every write.
    void setCacheWriteNotifier(std::function<void(const QString &)> notifier) {
        m_cacheWriteNotifier = std::move(notifier);
    }
    // Requests cancellation and returns immediately; unloadFinished reports completion.
    api::Result unload(const QString &pluginId, PluginRegistry &registry);
    QStringList unloadImpact(const QString &pluginId) const;
    // Nonblocking. Keep this loader and the registry alive until shutdownFinished.
    void requestShutdown();
    bool shutdownComplete() const { return m_shutdownComplete; }
    QStringList pendingInitializations() const;
signals:
    void stateChanged();
    void loadFinished(const QString &pluginId, bool success, const QString &message);
    void unloadFinished(const QString &pluginId, bool success, const QString &message);
    void shutdownProgress(const QStringList &pluginNames);
    void shutdownFinished();
private:
    struct LoadedPlugin {
        std::unique_ptr<QPluginLoader> library;
        std::unique_ptr<util::HostServicesImpl> services;
        QPointer<PluginRegistry> registry;
        QString id;
        util::CancellationSource cancellation;
        api::PluginLifecycleInterface *lifecycle = nullptr;
        api::PluginStopInterface *stopInterface = nullptr;
        std::unique_ptr<ServiceBroker> worker;
        QJsonObject manifest;
        QStringList dependencies;
        bool ready=false, keep=false, stopStarted=false, stopDone=false, stopFailed=false;
        quint64 instance=0;
        quint64 stopAttempt=0;
        std::unique_ptr<QTemporaryDir> cache;
        std::unique_ptr<QLockFile> cacheLock;
        QString path;
        bool unloading = false;
        bool releaseFailed = false;
        QString releaseError;
    };
    std::vector<LoadedPlugin> m_libraries;
    struct StagedPlugin;
    struct Loading {
        QString key;
        QPointer<PluginRegistry> registry;
        bool enable = true;
        QJsonObject manifest;
        util::CancellationSource cancellation;
        std::unique_ptr<QFutureWatcher<std::shared_ptr<StagedPlugin>>> watcher;
    };
    std::vector<Loading> m_loads;
    QThreadPool m_loadingPool;
    storage::SettingsStore *m_settings = nullptr;
    storage::StorageContext m_storage;
    package::CacheContext m_cacheContext = package::CacheContext::temporary();
    QVariant setting(const QString &key, const QVariant &fallback = {}) const;
    void setSetting(const QString &key, const QVariant &value);
    void removeSetting(const QString &key);
    void finishLoad(QFutureWatcher<std::shared_ptr<StagedPlugin>> *watcher);
    QThreadPool m_preparationPool;
    struct Preparation {
        QPointer<PluginRegistry> registry;
        QString id;
        QString name;
        std::unique_ptr<QFutureWatcher<api::CreateResult>> watcher;
    };
    std::vector<Preparation> m_preparations;
    bool m_busy = false;
    bool m_stopping = false;
    bool m_shutdownComplete = false;
    QTimer m_shutdownCheck;
    QStringList m_lastPending;
    void checkShutdown();
    void prepare(api::CreateTask task, const QString &failure, const QString &pluginId,
                 const QString &pluginName, PluginRegistry &registry, util::CancellationToken cancellation);
    bool release(LoadedPlugin &plugin);
    void drainUnloads();
    QMap<QString, QVariantMap> m_catalog;
    struct ScannedInstall {
        QList<package::PackageResult> packages;
        storage::PluginDataStore::InstalledNameClaims claims;
    };
    struct Discovery {
        QStringList roots;
        QPointer<PluginRegistry> registry;
        bool enable=false;
        std::unique_ptr<QFutureWatcher<ScannedInstall>> watcher;
    };
    std::vector<Discovery> m_discoveries;
    storage::PluginDataStore::InstalledNameClaims m_installedNameClaims;
    QMap<QString,package::PackageResult> m_verified;
    struct Transaction { QString target; QStringList order; QPointer<PluginRegistry> registry; bool keep=false,enable=false; QMap<QString,package::PackageResult> packages; };
    std::vector<Transaction> m_transactions;
    QSet<QString> m_failedNodes;
    quint64 m_instanceSerial=0;
    QSet<QString> m_cancelledPaths;
    QVariantList m_pendingDeletions;
    QStringList m_pendingDeletionIssues;
    std::function<void(const QString &)> m_cacheWriteNotifier;
    api::Result discover(const QStringList &paths,const QStringList &roots,PluginRegistry &,bool enable);
    void finishDiscovery(QFutureWatcher<ScannedInstall> *);
    api::Result startFile(const package::PackageResult &,PluginRegistry &,bool enable);
    void advance();
    void scheduleAdvance();
    void nodeFinished(const QString &,bool,const QString &);
    void stopPlugin(LoadedPlugin &);
    bool hasDependent(const QString &) const;
    void finishPreparation(QPointer<PluginRegistry> registry, const QString &pluginId,
                           const api::CreateResult &result, QPointer<QObject> preparation);
};
}
