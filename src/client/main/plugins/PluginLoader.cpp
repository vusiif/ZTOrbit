#include "PluginLoader.h"
#include "PluginRegistry.h"
#include "HostServicesImpl.h"
#include <ztgui/PluginInterface.h>
#include <QDir>
#include <QJsonObject>
#include <QLibrary>
#include <QPluginLoader>
#include <QtConcurrent/QtConcurrentRun>
#include <QElapsedTimer>
#include <QPointer>
#include <QDebug>
#include <QTimer>
#include <QScopedValueRollback>
#include <QCoreApplication>
#include <QThread>
#include <algorithm>
#include <exception>

namespace vusiif::ztgui::plugins {
using namespace vusiif::ztgui::api;
PluginLoader::PluginLoader()
{
    m_preparationPool.setMaxThreadCount(2);
    m_shutdownCheck.setInterval(20);
    connect(&m_shutdownCheck, &QTimer::timeout, this, &PluginLoader::checkShutdown);
}
PluginLoader::~PluginLoader()
{
    m_busy = true;
    m_stopping = true;
    m_shutdownCheck.stop();
    for (const auto &plugin : m_libraries) plugin.cancellation.requestCancellation();
    for (auto &preparation : m_preparations) QObject::disconnect(preparation.watcher.get(), nullptr, this, nullptr);
    m_preparationPool.waitForDone();
    m_preparations.clear();
    for (auto &plugin : m_libraries) {
        if (plugin.registry) plugin.registry->unregisterPlugin(plugin.id);
        release(plugin);
    }
}
QStringList PluginLoader::scan(const QString &directory, PluginRegistry &registry)
{
    QStringList messages;
    if (m_busy || m_stopping || QThread::currentThread() != thread()) return messages;
    const QScopedValueRollback<bool> guard(m_busy, true);
    const QDir dir(directory);
    if (!dir.exists()) return messages;
    for (const auto &file : dir.entryInfoList(QDir::Files, QDir::Name)) {
        if (m_stopping) break;
        if (!QLibrary::isLibrary(file.fileName())) continue;
        auto loader = std::make_unique<QPluginLoader>(file.absoluteFilePath());
        loader->setLoadHints(QLibrary::LoadHints{});
        const auto metadata = loader->metaData();
        if (metadata.value("IID").toString() != QLatin1String(ZTGUI_PLUGIN_IID)) continue;
        const auto info = metadata.value("MetaData").toObject();
        QString error;
        QString pluginId;
        QString pluginName;
        QObject *instance = nullptr;
        PluginLifecycleInterface *lifecycle = nullptr;
        if (info.value("apiVersion").toString() != QLatin1String(ApiVersion)) {
            error = QStringLiteral("API version mismatch");
        } else {
            try {
                instance = loader->instance();
                auto *entry = qobject_cast<PluginInterface *>(instance);
                if (!entry) {
                    error = loader->errorString();
                    if (error.isEmpty()) error = QStringLiteral("PluginInterface is missing");
                }
                else {
                    auto definition = entry->definition();
                    pluginId = definition.info.id;
                    pluginName = definition.info.name;
                    lifecycle = qobject_cast<PluginLifecycleInterface *>(instance);
                    const auto result = registry.registerPlugin(std::move(definition), lifecycle,
                        lifecycle != nullptr);
                    if (!result.ok()) error = result.message;
                }
            } catch (const std::exception &e) { error = QStringLiteral("Plugin entry threw an exception: %1").arg(QString::fromUtf8(e.what())); }
            catch (...) { error = QStringLiteral("Plugin entry threw an exception"); }
        }
        if (!error.isEmpty()) {
            messages << QStringLiteral("[插件] %1: %2").arg(file.fileName(), error);
            loader->unload();
        } else {
            messages << QStringLiteral("[插件] 已加载 %1").arg(file.fileName());
            LoadedPlugin plugin;
            plugin.library = std::move(loader);
            plugin.services = std::make_unique<util::HostServicesImpl>(pluginName, QString(), pluginId);
            plugin.registry = &registry;
            plugin.id = pluginId;
            plugin.lifecycle = lifecycle;
            m_libraries.push_back(std::move(plugin));
            auto &loaded = m_libraries.back();
            CreateTask task;
            QString failure;
            try {
                const QScopedValueRollback<bool> creating(registry.m_busy, true);
                if (!lifecycle) continue;
                task = lifecycle->onCreate(*loaded.services, loaded.cancellation.token());
            } catch (const std::exception &e) { failure = QStringLiteral("onCreate 工厂抛出异常：%1").arg(QString::fromUtf8(e.what())); }
            catch (...) { failure = QStringLiteral("onCreate 工厂抛出异常"); }
            if (failure.isEmpty() && !task)
                registry.setPreparationState(pluginId, PreparationState::Ready);
            else prepare(std::move(task), failure, pluginId, pluginName, registry, loaded.cancellation.token());
        }
    }
    return messages;
}

void PluginLoader::prepare(CreateTask task, const QString &failure, const QString &pluginId,
                          const QString &pluginName, PluginRegistry &registry, util::CancellationToken cancellation)
{
    registry.setPreparationState(pluginId, PreparationState::Preparing, QStringLiteral("插件准备中…"));
    if (!task) {
        registry.setPreparationState(pluginId, PreparationState::Failed, failure);
        qWarning().noquote() << "[插件]" << pluginId << failure;
        return;
    }
    auto watcher = std::make_unique<QFutureWatcher<CreateResult>>();
    auto *observer = watcher.get();
    const QPointer<PluginRegistry> target(&registry);
    connect(observer, &QFutureWatcher<CreateResult>::finished, this, [this, observer, target, pluginId] {
        finishPreparation(target, pluginId, observer->result(), observer);
        if (m_stopping) checkShutdown();
    });
    observer->setFuture(QtConcurrent::run(&m_preparationPool, [task = std::move(task), pluginId, cancellation]() mutable {
        if (cancellation.isCancellationRequested())
            return CreateResult{false, QStringLiteral("初始化已取消")};
        QElapsedTimer timer; timer.start();
        CreateResult result;
        try { result = task(); }
        catch (const std::exception &e) { result = {false, QString::fromUtf8(e.what())}; }
        catch (...) { result = {false, QStringLiteral("onCreate 初始化抛出异常")}; }
        if (!result.success && result.message.isEmpty()) result.message = QStringLiteral("插件初始化失败");
        qInfo().noquote() << "[插件]" << pluginId << "onCreate" << timer.elapsed() << "ms";
        return result;
    }));
    m_preparations.push_back({&registry, pluginId, pluginName, std::move(watcher)});
}

void PluginLoader::finishPreparation(QPointer<PluginRegistry> registry, const QString &pluginId,
                                    const CreateResult &result, QPointer<QObject> preparation)
{
    if (m_stopping || !registry || !preparation) return;
    if (m_busy) {
        QTimer::singleShot(10, preparation.data(), [this, registry, pluginId, result, preparation] {
            finishPreparation(registry, pluginId, result, preparation);
        });
        return;
    }
    const auto update = registry->setPreparationState(pluginId,
        result.success ? PreparationState::Ready : PreparationState::Failed, result.message);
    // A page factory may process nested events while the registry guards reentry.
    // Retry once the factory returns rather than leaving a finished plugin disabled.
    if (update.error == Error::Busy) {
        QTimer::singleShot(10, preparation.data(), [this, registry, pluginId, result, preparation] {
            finishPreparation(registry, pluginId, result, preparation);
        });
        return;
    }
    if (!result.success) qWarning().noquote() << "[插件]" << pluginId << "onCreate:" << result.message;
}

void PluginLoader::release(LoadedPlugin &plugin)
{
    plugin.services->stopRequests();
    if (plugin.lifecycle) {
        const auto destroy = [&] {
            try { plugin.lifecycle->onDestroy(); }
            catch (const std::exception &e) { qWarning() << "[插件] onDestroy:" << e.what(); }
            catch (...) { qWarning() << "[插件] onDestroy 抛出未知异常"; }
        };
        if (plugin.registry) {
            const QScopedValueRollback<bool> destroying(plugin.registry->m_busy, true);
            destroy();
        } else destroy();
        plugin.lifecycle = nullptr;
    }
    plugin.library->unload();
    plugin.services.reset();
}

Result PluginLoader::unload(const QString &pluginId, PluginRegistry &registry)
{
    if (QThread::currentThread() != thread()) return {Error::WrongThread, QStringLiteral("卸载必须在 GUI 线程执行")};
    if (m_busy || m_stopping) return {Error::Busy, QStringLiteral("插件加载器正在执行生命周期或退出")};
    const auto entry = std::find_if(m_libraries.begin(), m_libraries.end(), [&](const auto &item) {
        return item.id == pluginId && item.registry == &registry;
    });
    if (entry == m_libraries.end()) return {Error::NotFound, QStringLiteral("动态插件未加载")};
    if (const auto result = registry.checkMutation(); !result.ok()) return result;
    const QScopedValueRollback<bool> guard(m_busy, true);
    entry->cancellation.requestCancellation();
    for (auto it = m_preparations.begin(); it != m_preparations.end();) {
        if (it->id == pluginId && it->registry == &registry) {
            disconnect(it->watcher.get(), nullptr, this, nullptr);
            it->watcher->waitForFinished();
            it = m_preparations.erase(it);
        } else ++it;
    }
    const auto result = registry.unregisterPlugin(pluginId);
    if (!result.ok() && result.error != Error::NotFound) return result;
    release(*entry);
    m_libraries.erase(entry);
    return {};
}

QStringList PluginLoader::pendingInitializations() const
{
    QStringList pending;
    for (const auto &preparation : m_preparations) {
        if (!preparation.watcher->isFinished()) pending.append(preparation.name);
    }
    pending.removeDuplicates();
    return pending;
}

void PluginLoader::requestShutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_stopping) return;
    m_stopping = true;
    for (const auto &plugin : m_libraries) plugin.cancellation.requestCancellation();
    m_lastPending = pendingInitializations();
    emit shutdownProgress(m_lastPending);
    m_shutdownCheck.start();
    QTimer::singleShot(0, this, &PluginLoader::checkShutdown);
}

void PluginLoader::checkShutdown()
{
    if (!m_stopping || m_shutdownComplete || m_busy) return;
    const auto pending = pendingInitializations();
    if (pending != m_lastPending) {
        m_lastPending = pending;
        emit shutdownProgress(pending);
    }
    // Future completion can precede the worker's final stack unwinding. A zero
    // deadline also confirms all runnables/captures are gone without blocking UI.
    if (!pending.isEmpty() || !m_preparationPool.waitForDone(0)) return;
    m_shutdownCheck.stop();
    m_shutdownComplete = true;
    emit shutdownFinished();
}
}
