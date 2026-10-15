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
    // Requests cancellation and returns immediately; unloadFinished reports completion.
    api::Result unload(const QString &pluginId, PluginRegistry &registry);
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
    void finishPreparation(QPointer<PluginRegistry> registry, const QString &pluginId,
                           const api::CreateResult &result, QPointer<QObject> preparation);
};
}
