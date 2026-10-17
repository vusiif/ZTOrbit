#pragma once
#include <QObject>
#include <QStringList>
#include <QThreadPool>
#include <QFutureWatcher>
#include <QPointer>
#include <QTimer>
#include <ztgui/PluginLifecycle.h>
#include <memory>
#include <vector>
#include <QVariantList>
#include <QTemporaryDir>
#include <QLockFile>
#include <ztgui/PluginStop.h>
#include "PluginPackage.h"
#include "ServiceBroker.h"
class QPluginLoader;
namespace vusiif::ztgui::util { class HostServicesImpl; }
namespace vusiif::ztgui::plugins {
class PluginRegistry;

class PluginLoader final : public QObject {
    Q_OBJECT
public:
    PluginLoader();
    ~PluginLoader();
    QStringList scan(const QString &directory, PluginRegistry &registry);
    // Accepts a background load; loadFinished reports readiness or failure.
    api::Result loadFile(const QString &path, PluginRegistry &registry, bool enable = true);
    QVariantList catalog() const;
    api::Result removeFile(const QString &path, const QString &pluginDirectory,
                           const QString &dataRoot = {});
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
    struct Discovery {
        QStringList roots;
        QPointer<PluginRegistry> registry;
        bool enable=false;
        std::unique_ptr<QFutureWatcher<QList<package::PackageResult>>> watcher;
    };
    std::vector<Discovery> m_discoveries;
    QMap<QString,package::PackageResult> m_verified;
    struct Transaction { QString target; QStringList order; QPointer<PluginRegistry> registry; bool keep=false,enable=false; QMap<QString,package::PackageResult> packages; };
    std::vector<Transaction> m_transactions;
    QSet<QString> m_failedNodes;
    quint64 m_instanceSerial=0;
    QSet<QString> m_cancelledPaths;
    api::Result discover(const QStringList &paths,const QStringList &roots,PluginRegistry &,bool enable);
    void finishDiscovery(QFutureWatcher<QList<package::PackageResult>> *);
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
