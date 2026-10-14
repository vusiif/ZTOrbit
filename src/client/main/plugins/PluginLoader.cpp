#include "PluginLoader.h"
#include "PluginRegistry.h"
#include "HostServicesImpl.h"
#include "PluginPackage.h"
#include "PluginRuntime.h"
#include <QSettings>
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
    connect(&m_shutdownCheck, &QTimer::timeout, this, [this] { drainUnloads(); checkShutdown(); });
    package::cleanStaleCache();
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
        if(!release(plugin)) { plugin.library.release(); plugin.cacheLock.release(); }
    }
}
QStringList PluginLoader::scan(const QString &directory, PluginRegistry &registry)
{
    QStringList messages;
    if(m_busy || m_stopping || QThread::currentThread()!=thread()) return messages;
    for(const auto &file:QDir(directory).entryInfoList({"*.ztplugin"},QDir::Files|QDir::NoSymLinks,QDir::Name)) {
        const auto result=loadFile(file.absoluteFilePath(),registry,false);
        messages << QString("[plugin] %1: %2").arg(file.fileName(),result.ok()?"loaded or disabled":result.message);
    }
    return messages;
}
QVariantList PluginLoader::catalog() const {
    QVariantList list; for(const auto &entry:m_catalog) list.append(entry); return list;
}
Result PluginLoader::loadFile(const QString &path,PluginRegistry &registry,bool enable) {
    if(QThread::currentThread()!=thread()) return {Error::WrongThread,"Load must run on the GUI thread"};
    if(m_busy || m_stopping) return {Error::Busy,"Loader is busy or shutting down"};
    const QScopedValueRollback<bool> guard(m_busy,true);
    auto pkg=package::inspect(path);
    QString error=pkg.error;
    if(error.isEmpty()) error=package::validate(pkg.info,"client",ApiVersion,ZTGUI_PLUGIN_IID);
    if(error.isEmpty()) error=package::validateRuntime(pkg);
    const auto id=pkg.info["id"].toString(); const auto key=id.toLower();
    if(error.isEmpty()) {
        for(const auto &loaded:m_libraries) if(loaded.id.compare(id,Qt::CaseInsensitive)==0) return {Error::DuplicatePlugin,"Plugin is loaded or still stopping"};
        for(const auto &registered:registry.plugins()) {
            if(registered.id.compare(id,Qt::CaseInsensitive)==0) return {Error::DuplicatePlugin,"Plugin ID already registered"};
            if(registered.name.compare(pkg.info["name"].toString(),Qt::CaseInsensitive)==0) return {Error::DuplicatePlugin,"Plugin data directory/name already used"};
        }
    }
    QString catalogKey=key.isEmpty()?path:key;
    if(m_catalog.contains(catalogKey) && m_catalog[catalogKey]["path"].toString()!=path) catalogKey=path;
    auto &row=m_catalog[catalogKey];
    row={{"id",id},{"name",pkg.info["name"].toString()},{"version",pkg.info["version"].toString()},
         {"api",pkg.info["apiVersion"].toString()},{"path",path},{"loaded",false},{"stopping",false},{"loadable",error.isEmpty()},{"message",error}};
    if(!error.isEmpty()) { emit stateChanged(); return {Error::InvalidPlugin,error}; }
    if(!enable && QSettings().value("plugins/disabled/"+key,false).toBool()) { row["message"]="Disabled"; emit stateChanged(); return {}; }
    LoadedPlugin loaded; loaded.path=path; QString library;
    error=package::extract(pkg,loaded.cache,library,loaded.cacheLock);
    if(error.isEmpty()) {
        loaded.library=std::make_unique<QPluginLoader>();
        // Qt 6.8 only ORs hints after association with a file; clear before setFileName.
        loaded.library->setLoadHints(QLibrary::LoadHints{}); loaded.library->setFileName(library);
        const auto metadata=loaded.library->metaData();
        if(metadata["IID"].toString()!=QLatin1String(ZTGUI_PLUGIN_IID) || metadata["MetaData"].toObject()["apiVersion"].toString()!=QLatin1String(ApiVersion)) error="Inner plugin metadata mismatch";
    }
    if(error.isEmpty()) error=package::activateRuntime(pkg,loaded.cache->path());
    if(error.isEmpty()) {
        try {
            auto *instance=loaded.library->instance(); auto *entry=qobject_cast<PluginInterface *>(instance);
            if(!entry) error=loaded.library->errorString();
            else {
                auto definition=entry->definition(); const auto &info=definition.info;
                if(info.id!=id || info.name!=pkg.info["name"].toString() || info.version!=pkg.info["version"].toString()) error="Package and plugin definition differ";
                else {
                    loaded.lifecycle=qobject_cast<PluginLifecycleInterface *>(instance);
                    const auto result=registry.registerPlugin(std::move(definition),loaded.lifecycle,loaded.lifecycle!=nullptr);
                    if(!result.ok()) error=result.message;
                }
            }
        } catch(const std::exception &e) { error=QString::fromUtf8(e.what()); }
        catch(...) { error="Plugin entry threw an exception"; }
    }
    if(!error.isEmpty()) {
        if(loaded.library && !loaded.library->unload() && loaded.library->isLoaded()) { loaded.cache->setAutoRemove(false); loaded.library.release(); loaded.cacheLock.release(); }
        else loaded.library.reset();
        row["message"]=error; emit stateChanged(); return {Error::InvalidPlugin,error};
    }
    loaded.id=id; loaded.registry=&registry;
    loaded.services=std::make_unique<util::HostServicesImpl>(pkg.info["name"].toString(),QString(),id);
    m_libraries.push_back(std::move(loaded)); auto &plugin=m_libraries.back();
    row["loaded"]=true; row["message"]=plugin.lifecycle?"Preparing":"Ready";
    CreateTask task; QString failure;
    try { const QScopedValueRollback<bool> creating(registry.m_busy,true);
        if(plugin.lifecycle) task=plugin.lifecycle->onCreate(*plugin.services,plugin.cancellation.token());
    } catch(const std::exception &e) { failure=QString::fromUtf8(e.what()); }
    catch(...) { failure="onCreate threw an exception"; }
    if(failure.isEmpty()&&!task) { registry.setPreparationState(id,PreparationState::Ready); row["message"]="Ready"; }
    else prepare(std::move(task),failure,id,pkg.info["name"].toString(),registry,plugin.cancellation.token());
    if(enable) QSettings().remove("plugins/disabled/"+key);
    emit stateChanged(); return {};
}

void PluginLoader::prepare(CreateTask task, const QString &failure, const QString &pluginId,
                          const QString &pluginName, PluginRegistry &registry, util::CancellationToken cancellation)
{
    registry.setPreparationState(pluginId, PreparationState::Preparing, QStringLiteral("插件准备中…"));
    if (!task) {
        QTimer::singleShot(0,this,[this,pluginId] { for(auto &p:m_libraries) if(p.id==pluginId) { p.unloading=true; p.services->stopRequests(); m_shutdownCheck.start(); } });
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
    for(const auto &p:m_libraries) if(p.id==pluginId && p.unloading) return;
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
    auto &row=m_catalog[pluginId.toLower()]; row["message"]=result.success?QString("Ready"):QString(result.message.constData(),result.message.size()); emit stateChanged();
    if(!result.success) {
        for(auto &p:m_libraries) if(p.id==pluginId) { p.unloading=true; p.services->stopRequests(); }
        m_shutdownCheck.start();
    }
}

bool PluginLoader::release(LoadedPlugin &plugin)
{
    if(plugin.releaseFailed) return false;
    plugin.services->stopRequests();
    if (plugin.lifecycle) {
        const auto destroy = [&] {
            try { plugin.lifecycle->onDestroy(); }
            catch (const std::exception &e) { plugin.releaseError="Plugin cleanup failed: "+QString::fromUtf8(e.what()); }
            catch (...) { plugin.releaseError="Plugin cleanup threw an exception"; }
        };
        if (plugin.registry) {
            const QScopedValueRollback<bool> destroying(plugin.registry->m_busy, true);
            destroy();
        } else destroy();
        plugin.lifecycle = nullptr;
        if(!plugin.releaseError.isEmpty()) { plugin.releaseFailed=true; plugin.cache->setAutoRemove(false); return false; }
    }
    if(!plugin.library->unload() && plugin.library->isLoaded()) {
        plugin.releaseError="Dynamic library unload failed: "+plugin.library->errorString();
        plugin.releaseFailed=true; plugin.cache->setAutoRemove(false); return false;
    }
    if(package::businessLibraryMapped(plugin.library->fileName())) {
        plugin.releaseError="Business library remains mapped; restart required";
        plugin.releaseFailed=true; plugin.cache->setAutoRemove(false); return false;
    }
    plugin.library.reset(); plugin.services.reset(); plugin.cacheLock.reset(); package::discardCache(plugin.cache); return true;
}

Result PluginLoader::unload(const QString &pluginId,PluginRegistry &registry) {
    if(QThread::currentThread()!=thread()) return {Error::WrongThread,"Unload must run on GUI thread"};
    if(m_busy||m_stopping) return {Error::Busy,"Loader is busy or shutting down"};
    for(auto &p:m_libraries) if(p.id==pluginId&&p.registry==&registry) {
        if(p.unloading||p.releaseFailed) return {Error::Busy,"Plugin is stopping or could not unload"};
        if(const auto result=registry.checkMutation();!result.ok()) return result;
        p.unloading=true; p.cancellation.requestCancellation(); p.services->stopRequests();
        if(registry.m_active.plugin.id==pluginId) registry.activateMenu(ZeroTierMenuId);
        registry.setPreparationState(pluginId,PreparationState::Stopping,"Stopping");
        auto &row=m_catalog[pluginId.toLower()]; row["stopping"]=true; row["message"]="Stopping";
        QSettings().setValue("plugins/disabled/"+pluginId.toLower(),true);
        m_shutdownCheck.start(); emit stateChanged(); return {};
    }
    return {Error::NotFound,"Plugin is not loaded"};
}
void PluginLoader::drainUnloads() {
    if(m_busy||!m_preparationPool.waitForDone(0)) return;
    const QScopedValueRollback<bool> guard(m_busy,true);
    for(auto i=m_libraries.begin();i!=m_libraries.end();) {
        if(!i->unloading||i->releaseFailed) { ++i; continue; }
        bool pending=false;
        for(auto &task:m_preparations) if(task.id==i->id&&!task.watcher->isFinished()) pending=true;
        if(pending || i->services->hasCallbacks()) { ++i; continue; }
        for(auto t=m_preparations.begin();t!=m_preparations.end();) {
            if(t->id==i->id) { disconnect(t->watcher.get(),nullptr,this,nullptr); t=m_preparations.erase(t); } else ++t;
        }
        if(i->registry) {
            const auto result=i->registry->unregisterPlugin(i->id);
            if(result.error==Error::Busy) { ++i; continue; }
        }
        const auto id=i->id; const bool ok=release(*i);
        auto &row=m_catalog[id.toLower()]; row["loaded"]=!ok; row["stopping"]=false;
        if(ok) { if(row["message"].toString()=="Stopping" || m_stopping) row["message"]="Disabled"; i=m_libraries.erase(i); }
        else { row["message"]=i->releaseError; ++i; }
        emit stateChanged(); emit unloadFinished(id,ok,row["message"].toString());
    }
    if(!m_stopping&&std::none_of(m_libraries.begin(),m_libraries.end(),[](const LoadedPlugin &p) { return p.unloading&&!p.releaseFailed; })) m_shutdownCheck.stop();
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
    for (auto &plugin : m_libraries) { plugin.cancellation.requestCancellation(); plugin.services->stopRequests(); plugin.unloading=true; }
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
    drainUnloads();
    if(std::any_of(m_libraries.begin(),m_libraries.end(),[](const LoadedPlugin &p) { return !p.releaseFailed; })) return;
    m_shutdownCheck.stop();
    m_shutdownComplete = true;
    emit shutdownFinished();
}
}
