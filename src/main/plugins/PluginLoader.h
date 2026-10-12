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
    // Internal unload building block; no directory watching or hot-load UI.
    api::Result unload(const QString &pluginId, PluginRegistry &registry);
    // Nonblocking. Keep this loader and the registry alive until shutdownFinished.
    void requestShutdown();
    bool shutdownComplete() const { return m_shutdownComplete; }
    QStringList pendingInitializations() const;
signals:
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
    };
    std::vector<LoadedPlugin> m_libraries;
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
    void release(LoadedPlugin &plugin);
    void finishPreparation(QPointer<PluginRegistry> registry, const QString &pluginId,
                           const api::CreateResult &result, QPointer<QObject> preparation);
};
}
