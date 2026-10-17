#include "PluginLoader.h"
#include "PluginRegistry.h"
#include "HostServicesImpl.h"
#include "PluginPackage.h"
#include "PluginRuntime.h"
#include "PluginDependencies.h"
#include <QJsonArray>
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
#include <QDirIterator>
#include <QMutex>
#include <QMutexLocker>
#include <algorithm>
#include <exception>

namespace vusiif::ztgui::plugins {
using namespace vusiif::ztgui::api;
struct PluginLoader::StagedPlugin {
    package::PackageResult package;
    LoadedPlugin plugin;
    QString error;
    bool loadable = false;
    bool disabled = false;
};

PluginLoader::PluginLoader()
{
    m_loadingPool.setMaxThreadCount(1);
    m_preparationPool.setMaxThreadCount(2);
    m_shutdownCheck.setInterval(20);
    connect(&m_shutdownCheck, &QTimer::timeout, this, [this] { drainUnloads(); checkShutdown(); });
    connect(this,&PluginLoader::loadFinished,this,&PluginLoader::nodeFinished);
    connect(this,&PluginLoader::unloadFinished,this,[this] { scheduleAdvance(); });
    package::cleanStaleCache();
}
PluginLoader::~PluginLoader()
{
    m_busy = true;
    m_stopping = true;
    m_shutdownCheck.stop();
    for (auto &load : m_loads) {
        load.cancellation.requestCancellation();
        QObject::disconnect(load.watcher.get(), nullptr, this, nullptr);
    }
    m_loadingPool.waitForDone();
    for(auto &discovery:m_discoveries) disconnect(discovery.watcher.get(),nullptr,this,nullptr);
    m_discoveries.clear();
    for(auto &load:m_loads) {
        auto &plugin=load.watcher->result()->plugin;
        if(plugin.library && !plugin.library->unload() && plugin.library->isLoaded()) {
            plugin.cache->setAutoRemove(false); plugin.library.release(); plugin.cacheLock.release();
        }
    }
    m_loads.clear();
    for (const auto &plugin : m_libraries) plugin.cancellation.requestCancellation();
    for (auto &preparation : m_preparations) QObject::disconnect(preparation.watcher.get(), nullptr, this, nullptr);
    m_preparationPool.waitForDone();
    m_preparations.clear();
    for (auto it=m_libraries.rbegin();it!=m_libraries.rend();++it) { auto &plugin=*it;
        if (plugin.registry) plugin.registry->unregisterPlugin(plugin.id);
        if(plugin.worker) {
            // Emergency destruction still owns and terminates its child tree.
            // Normal shutdown takes the fully asynchronous, observed-exit path.
            plugin.worker.reset(); plugin.services.reset(); plugin.cacheLock.reset(); package::discardCache(plugin.cache); continue;
        }
        if((plugin.stopInterface && !plugin.stopDone) || plugin.services->hasCallbacks() || plugin.services->serviceClient.hasCallbacks()) {
            // Never unload an in-process DLL whose asynchronous stop was skipped.
            plugin.services->stopRequests(); plugin.services->serviceClient.closeAll();
            plugin.cache->setAutoRemove(false); plugin.library.release(); plugin.services.release(); plugin.cacheLock.release(); continue;
        }
        if(!release(plugin)) { plugin.library.release(); plugin.worker.release(); plugin.services.release(); plugin.cacheLock.release(); }
    }
}
QStringList PluginLoader::scan(const QString &directory, PluginRegistry &registry)
{
    QStringList messages;
    if(m_busy || m_stopping || QThread::currentThread()!=thread()) return messages;
    QStringList paths;
    for(const auto &file:QDir(directory).entryInfoList({"*.ztplugin"},QDir::Files|QDir::NoSymLinks,QDir::Name)) paths.append(file.absoluteFilePath());
    if(!paths.isEmpty()) { const auto result=discover(paths,paths,registry,false); messages.append(result.ok()?"[plugin] catalog verification queued":result.message); }
    return messages;
}
QVariantList PluginLoader::catalog() const {
    QVariantList list; for(auto entry:m_catalog) {
        for(const auto &key:{"loadable","loaded","loading","stopping","disabled","forcedTermination"}) entry[key]=entry[key].toBool();
        QStringList users; for(const auto &p:m_libraries) if(p.dependencies.contains(entry["id"].toString().toCaseFolded())) users.append(p.id);
        entry["usedBy"]=users; list.append(entry);
    } return list;
}
Result PluginLoader::loadFile(const QString &path,PluginRegistry &registry,bool enable) {
    QStringList paths;
    const auto absolute=QFileInfo(path).absoluteFilePath();
    for(const auto &file:QDir(QFileInfo(absolute).absolutePath()).entryInfoList({"*.ztplugin"},QDir::Files|QDir::NoSymLinks,QDir::Name)) paths.append(file.absoluteFilePath());
    if(!paths.contains(absolute)) paths.append(absolute);
    return discover(paths,{absolute},registry,enable);
}
Result PluginLoader::discover(const QStringList &paths,const QStringList &roots,PluginRegistry &registry,bool enable) {
    if(QThread::currentThread()!=thread()) return {Error::WrongThread,"Load must run on GUI thread"};
    if(m_busy || m_stopping) return {Error::Busy,"Loader is busy or shutting down"};
    for(const auto &root:roots) {
        for(auto &d:m_discoveries) if(d.roots.contains(root))
            return enable ? Result{Error::DuplicatePlugin,QStringLiteral("该插件正在加载，请勿重复加载。")} : Result{};
        for(auto &t:m_transactions) if(m_verified.value(t.target).path==root && m_catalog.value(t.target).value("loading").toBool())
            return enable ? Result{Error::DuplicatePlugin,QStringLiteral("该插件正在加载，请勿重复加载。")} : Result{};
        for(const auto &row:m_catalog) if(row["path"]==root && (row["loaded"].toBool() || (row["loading"].toBool() && row["message"]!="Verifying") || row["stopping"].toBool())) {
            if(!enable) continue;
            return {Error::DuplicatePlugin,QStringLiteral("同 ID 插件已加载、正在加载或正在卸载，无法重复加载。")};
        }
    }
    auto allPaths=paths;
    for(const auto &row:m_catalog) if(QFileInfo(row["path"].toString()).isFile() && !allPaths.contains(row["path"].toString())) allPaths.append(row["path"].toString());
    for(const auto &path:allPaths) {
        bool exists=false; for(auto &row:m_catalog) if(row["path"]==path) { exists=true; break; }
        if(!exists) m_catalog[path]={{"path",path},{"id",QString()},{"version",QString()},{"api",QString()},{"name",QFileInfo(path).completeBaseName()},{"loading",true},{"message","Verifying"},{"loadable",false}};
        else for(auto &row:m_catalog) if(row["path"]==path && !row["loaded"].toBool()) { row["loading"]=true; row["message"]="Verifying"; }
    }
    for(const auto &root:roots) m_cancelledPaths.remove(root);
    Discovery d; d.roots=roots; d.registry=&registry; d.enable=enable; d.watcher=std::make_unique<QFutureWatcher<QList<package::PackageResult>>>();
    auto *observer=d.watcher.get();
    connect(observer,&QFutureWatcher<QList<package::PackageResult>>::finished,this,[this,observer] { finishDiscovery(observer); });
    observer->setFuture(QtConcurrent::run(&m_loadingPool,[paths=allPaths] {
        QList<package::PackageResult> packages;
        for(const auto &path:paths) {
            auto p=package::inspect(path);
            if(p.ok()) p.error=package::authenticate(p);
            if(p.ok()) p.error=package::validate(p.info,"client",ApiVersion,ZTGUI_PLUGIN_IID);
            if(p.ok()) p.error=package::validateRuntime(p);
            packages.append(std::move(p));
        } return packages;
    }));
    m_discoveries.push_back(std::move(d)); emit stateChanged(); return {};
}
void PluginLoader::finishDiscovery(QFutureWatcher<QList<package::PackageResult>> *watcher) {
    const auto it=std::find_if(m_discoveries.begin(),m_discoveries.end(),[&](const Discovery &d) { return d.watcher.get()==watcher; });
    if(it==m_discoveries.end()) return;
    if(m_busy) { QTimer::singleShot(10,this,[this,watcher] { finishDiscovery(watcher); }); return; }
    const auto roots=it->roots; const auto registry=it->registry; const auto enable=it->enable; auto packages=watcher->result(); m_discoveries.erase(it);
    QMap<QString,QStringList> identities;
    for(const auto &p:packages) if(p.ok()) identities[p.info.value("id").toString().toCaseFolded()].append(p.path);
    QMap<QString,QJsonObject> graph; QSet<QString> disabled;
    for(auto &p:packages) {
        const auto id=p.info.value("id").toString(); const auto key=id.toCaseFolded();
        if(identities[key].size()>1) p.error="Duplicate normalized plugin ID: "+key+" ("+identities[key].join(", ")+")";
        QString rowKey=p.path; for(auto r=m_catalog.begin();r!=m_catalog.end();++r) if(r.value()["path"]==p.path) { rowKey=r.key(); break; }
        const bool running=m_catalog[rowKey]["loaded"].toBool();
        bool reserved=false; for(const auto &load:m_loads) if(load.key==key) reserved=true;
        for(const auto &t:m_transactions) if(t.order.contains(key)) reserved=true;
        auto row=m_catalog.take(rowKey);
        if(!id.isEmpty()) { row["id"]=id; row["name"]=p.info.value("name").toString(); row["version"]=p.info.value("version").toString(); row["api"]=p.info.value("apiVersion").toString(); }
        row["dependencies"]=p.info.value("dependencies").toArray().toVariantList(); row["provides"]=p.info.value("provides").toArray().toVariantList(); row["execution"]=p.info.value("execution").toObject()["kind"].toString("in-process");
        if(!running && !reserved) { row["loading"]=false; row["loadable"]=p.ok(); row["message"]=p.ok()?"Not loaded":p.error; }
        const bool off=QSettings().value("plugins/disabled/"+key,false).toBool(); row["disabled"]=off;
        if(off && p.ok() && !running) row["message"]="Disabled";
        m_catalog.insert((identities[key].size()>1 && !(rowKey==key && (running || reserved))) || key.isEmpty()?p.path:key,row);
        if(p.ok()) { graph[key]=p.info; m_verified[key]=p; if(off) disabled.insert(key); }
        else m_verified.remove(key);
    }
    // Previously authenticated running providers may live in another import directory.
    for(const auto &p:m_libraries) if(!graph.contains(p.id.toCaseFolded()) && !identities.contains(p.id.toCaseFolded())) graph[p.id.toCaseFolded()]=p.manifest;
    for(const auto &root:roots) {
        const auto p=std::find_if(packages.cbegin(),packages.cend(),[&](const auto &p) { return p.path==root; });
        if(p==packages.cend()) continue;
        const auto id=p->info["id"].toString(),key=id.toCaseFolded();
        QString error=p->error;
        const bool cancelled=m_cancelledPaths.remove(root);
        if(cancelled && !key.isEmpty()) QSettings().setValue("plugins/disabled/"+key,true);
        if(m_stopping || !registry || cancelled) error="Plugin loading cancelled";
        if(!enable && disabled.contains(key) && error.isEmpty()) continue;
        const bool worker=p->info["execution"].toObject()["kind"]=="worker-process";
        const bool keep=enable || !worker || QSettings().value("plugins/retain/"+key,false).toBool();
        // A service starts through consumers or a saved explicit user intent.
        if(!enable && worker && !keep && error.isEmpty()) continue;
        auto plan=package::dependencyPlan(graph,key,disabled); if(error.isEmpty()) error=plan.error;
        if(error.isEmpty()) for(const auto &node:plan.order) for(const auto &live:m_libraries)
            if(live.id.toCaseFolded()==node && live.manifest!=graph[node]) error="Package changed while provider is running: "+node+"; unload it before loading consumers";
        if(error.isEmpty()) for(const auto &node:plan.order) for(const auto &load:m_loads)
            if(load.key==node && load.manifest!=graph[node]) error="Conflicting package in a concurrent load: "+node;
        if(!error.isEmpty()) {
            for(auto &row:m_catalog) if(row["path"]==root) { row["loading"]=false; row["message"]=error; }
            emit loadFinished(id,false,error); continue;
        }
        bool already=false; for(const auto &t:m_transactions) if(t.target==key) already=true;
        for(const auto &p:m_libraries) if(p.id.toCaseFolded()==key && p.ready) already=true;
        if(already) {
            if(enable) emit loadFinished(id,false,QStringLiteral("同 ID 插件已加载或正在加载，无法重复加载。"));
            continue;
        }
        for(const auto &node:plan.order) m_failedNodes.remove(node);
        Transaction transaction{key,plan.order,registry,keep,enable,{}};
        for(const auto &node:plan.order) transaction.packages[node]=m_verified.value(node);
        m_transactions.push_back(std::move(transaction));
        auto &row=m_catalog[key]; row["loading"]=true; row["message"]="Waiting for dependencies";
    }
    emit stateChanged(); scheduleAdvance(); checkShutdown();
}
void PluginLoader::scheduleAdvance() { QTimer::singleShot(0,this,&PluginLoader::advance); }
Result PluginLoader::removeFile(const QString &path, const QString &pluginDirectory, const QString &dataRoot) {
    if(QThread::currentThread()!=thread()) return {Error::WrongThread,"Remove must run on GUI thread"};
    if(m_busy || m_stopping || !m_discoveries.empty() || !m_loads.empty() || !m_transactions.empty())
        return {Error::Busy,QStringLiteral("插件正在处理操作，请稍后重试。")};
    const auto absolute=QFileInfo(path).absoluteFilePath();
    auto selected=m_catalog.end();
    for(auto it=m_catalog.begin();it!=m_catalog.end();++it) if(it.value()["path"]==absolute) { selected=it; break; }
    if(selected==m_catalog.end()) return {Error::NotFound,QStringLiteral("插件已不存在。")};
    const auto row=selected.value(); const auto id=row["id"].toString().toCaseFolded();
    const auto name=row["name"].toString();
    for(const auto &p:m_libraries) if(p.path==absolute || p.id.toCaseFolded()==id || p.manifest["name"].toString().compare(name,Qt::CaseInsensitive)==0)
        return {Error::Busy,QStringLiteral("请先卸载使用此 ID 或数据目录的插件。")};
    if(row["loaded"].toBool() || row["loading"].toBool() || row["stopping"].toBool())
        return {Error::Busy,QStringLiteral("请先等待插件完全卸载。")};
    const QFileInfo directory(pluginDirectory), file(absolute);
    const auto linked=[](const QFileInfo &info) { return info.isSymLink() || info.isJunction(); };
    if(linked(directory) || !directory.isDir() || linked(file) || (file.exists() && !file.isFile()) ||
       file.absolutePath()!=directory.absoluteFilePath() || file.suffix().compare("ztplugin",Qt::CaseInsensitive)!=0)
        return {Error::InvalidPlugin,QStringLiteral("只能删除插件目录中的普通 .ztplugin 文件。")};
    // Only authenticated metadata may select a plugin data directory.
    auto verified=package::inspect(absolute);
    const bool trusted=verified.ok() && package::authenticate(verified).isEmpty() &&
        verified.info["id"].toString().toCaseFolded()==id && verified.info["name"].toString()==name;
    const auto root=QFileInfo(dataRoot.isEmpty()?QDir(QCoreApplication::applicationDirPath()).filePath("plugins-data"):dataRoot);
    const auto data=QFileInfo(QDir(root.absoluteFilePath()).filePath(name));
    if(trusted) {
        if(name.isEmpty() || name=="." || name==".." || name.contains('/') || name.contains('\\') || name.contains(':') ||
           name.endsWith('.') || name.endsWith(' ') || name.contains(QChar('\0')) || linked(root) ||
           (root.exists() && !root.isDir()) || linked(data) || (data.exists() && !data.isDir()) || data.absolutePath()!=root.absoluteFilePath())
            return {Error::InvalidPlugin,QStringLiteral("插件数据目录无效，已取消删除。")};
        QDirIterator entries(data.absoluteFilePath(),QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
        while(entries.hasNext()) { entries.next(); if(linked(entries.fileInfo())) return {Error::InvalidPlugin,QStringLiteral("插件数据包含链接，已取消删除。")}; }
        if(data.exists() && !QDir(data.absoluteFilePath()).removeRecursively())
            return {Error::InvalidPlugin,QStringLiteral("无法完整删除插件数据，请检查文件占用后重试。")};
    }
    if(file.exists() && !QFile::remove(absolute)) return {Error::InvalidPlugin,QStringLiteral("无法删除插件文件，请检查文件占用后重试。")};
    m_catalog.erase(selected);
    if(m_verified.value(id).path==absolute) m_verified.remove(id);
    bool referenced=false; for(const auto &other:m_catalog) if(other["id"].toString().toCaseFolded()==id) referenced=true;
    if(!referenced && !id.isEmpty()) { QSettings().remove("plugins/disabled/"+id); QSettings().remove("plugins/retain/"+id); m_failedNodes.remove(id); }
    emit stateChanged(); return {};
}
bool PluginLoader::hasDependent(const QString &id) const {
    for(const auto &p:m_libraries) if(p.dependencies.contains(id.toCaseFolded())) return true;
    for(const auto &load:m_loads) for(const auto &d:load.manifest.value("dependencies").toArray())
        if(d.toObject()["pluginId"].toString().compare(id,Qt::CaseInsensitive)==0) return true;
    return false;
}
void PluginLoader::advance() {
    if(m_busy) { QTimer::singleShot(10,this,&PluginLoader::advance); return; }
    if(m_stopping) { m_transactions.clear(); return; }
    for(auto it=m_transactions.begin();it!=m_transactions.end();) {
        bool failed=!it->registry;
        QString reason;
        for(const auto &id:it->order) if(m_failedNodes.contains(id)) { failed=true; reason=m_catalog.value(id).value("message").toString(); break; }
        auto target=std::find_if(m_libraries.begin(),m_libraries.end(),[&](const LoadedPlugin &p) { return p.id.toCaseFolded()==it->target; });
        if(failed) {
            const auto id=it->target; it=m_transactions.erase(it);
            auto &row=m_catalog[id]; row["loading"]=false; if(!reason.isEmpty()) row["message"]="Dependency failed: "+reason;
            if(!m_failedNodes.contains(id)) emit loadFinished(row["id"].toString(),false,row["message"].toString());
            continue;
        }
        if(target!=m_libraries.end() && target->ready) {
            target->keep|=it->keep;
            if(it->enable) { QSettings().remove("plugins/disabled/"+it->target); QSettings().setValue("plugins/retain/"+it->target,true); m_catalog[it->target]["disabled"]=false; }
            it=m_transactions.erase(it); continue;
        }
        for(const auto &id:it->order) {
            auto loaded=std::find_if(m_libraries.begin(),m_libraries.end(),[&](const LoadedPlugin &p) { return p.id.toCaseFolded()==id; });
            if(loaded!=m_libraries.end()) {
                if(loaded->releaseFailed) { m_failedNodes.insert(id); scheduleAdvance(); break; }
                if(loaded->unloading || !loaded->ready) break;
                continue;
            }
            bool pending=false; for(const auto &load:m_loads) if(load.key==id) pending=true;
            if(pending) break;
            const auto result=startFile(it->packages.value(id),*it->registry,true);
            if(!result.ok()) { m_catalog[id]["message"]=result.message; m_failedNodes.insert(id); scheduleAdvance(); }
            break;
        }
        ++it;
    }
    // Rollback and automatic provider release share the same ownership rule.
    for(auto &p:m_libraries) {
        if(p.keep || p.unloading || hasDependent(p.id)) continue;
        bool reserved=false; for(const auto &t:m_transactions) if(t.order.contains(p.id.toCaseFolded())) reserved=true;
        if(!reserved) {
            p.unloading=true; p.cancellation.requestCancellation(); p.services->serviceClient.beginStop();
            auto &row=m_catalog[p.id.toCaseFolded()]; row["stopping"]=true; row["message"]="Stopping"; m_shutdownCheck.start();
        }
    }
    emit stateChanged();
}
void PluginLoader::nodeFinished(const QString &id,bool success,const QString &) {
    if(!success) m_failedNodes.insert(id.toCaseFolded());
    else for(auto &p:m_libraries) if(p.id==id) {
        p.ready=true;
        for(const auto &t:m_transactions) if(t.target==id.toCaseFolded()) {
            p.keep|=t.keep;
            if(t.enable) { QSettings().remove("plugins/disabled/"+t.target); QSettings().setValue("plugins/retain/"+t.target,true); m_catalog[t.target]["disabled"]=false; }
        }
    }
    scheduleAdvance();
}
Result PluginLoader::startFile(const package::PackageResult &selected,PluginRegistry &registry,bool enable) {
    const auto path=selected.path;
    if(QThread::currentThread()!=thread()) return {Error::WrongThread,"Load must run on the GUI thread"};
    if(m_busy || m_stopping) return {Error::Busy,"Loader is busy or shutting down"};
    const QScopedValueRollback<bool> guard(m_busy,true);
    const auto absolutePath=QFileInfo(path).absoluteFilePath();
    QString catalogKey=absolutePath;
    for(auto it=m_catalog.cbegin();it!=m_catalog.cend();++it) {
        if(it.value()["path"].toString()!=absolutePath) continue;
        if(it.value()["loaded"].toBool() || it.value()["stopping"].toBool())
            return {Error::DuplicatePlugin,"Plugin is loaded, loading or still stopping"};
        catalogKey=it.key(); break;
    }
    auto &row=m_catalog[catalogKey];
    if(row.isEmpty()) row={{"id",QString()},{"name",QFileInfo(path).completeBaseName()},
                          {"version",QString()},{"api",QString()}};
    row["path"]=absolutePath; row["loaded"]=false; row["loading"]=true;
    row["forcedTermination"]=false;
    row["stopping"]=false; row["loadable"]=true; row["message"]="Loading";
    Loading request; request.key=catalogKey;
    request.manifest=selected.info;
    request.registry=&registry; request.enable=enable;
    request.watcher=std::make_unique<QFutureWatcher<std::shared_ptr<StagedPlugin>>>();
    auto *observer=request.watcher.get();
    const auto cancellation=request.cancellation.token();
    const auto expected=selected.info;
    auto *guiThread=thread();
    connect(observer,&QFutureWatcher<std::shared_ptr<StagedPlugin>>::finished,this,[this,observer] { finishLoad(observer); });
    observer->setFuture(QtConcurrent::run(&m_loadingPool,[absolutePath,enable,cancellation,guiThread,expected] {
        auto staged=std::make_shared<StagedPlugin>();
        auto &pkg=staged->package; auto &loaded=staged->plugin; auto &error=staged->error;
        loaded.path=absolutePath;
        const auto cancelled=[&] {
            if(!cancellation.isCancellationRequested()) return false;
            error=QStringLiteral("插件加载已取消"); return true;
        };
        try {
            if(cancelled()) return staged;
            pkg=package::inspect(absolutePath); error=pkg.error;
            if(error.isEmpty() && pkg.info!=expected) error="Package changed after dependency planning";
            if(error.isEmpty()) error=package::validate(pkg.info,"client",ApiVersion,ZTGUI_PLUGIN_IID);
            if(error.isEmpty()) error=package::validateRuntime(pkg);
            if(!error.isEmpty() || cancelled()) return staged;
            staged->loadable=true;
            staged->disabled=!enable && QSettings().value("plugins/disabled/"+pkg.info.value("id").toString().toLower(),false).toBool();
            if(staged->disabled) return staged;
            QString library;
            error=package::extract(pkg,loaded.cache,library,loaded.cacheLock);
            if(!error.isEmpty() || cancelled()) return staged;
            loaded.manifest=pkg.info;
            for(const auto &d:pkg.info.value("dependencies").toArray()) loaded.dependencies.append(d.toObject()["pluginId"].toString().toCaseFolded());
            if(pkg.info.value("execution").toObject()["kind"]=="worker-process") return staged;
            loaded.library=std::make_unique<QPluginLoader>();
            // Clear hints before association; instance() must still run on the GUI thread.
            loaded.library->setLoadHints(QLibrary::LoadHints{}); loaded.library->setFileName(library);
            const auto metadata=loaded.library->metaData();
            if(metadata["IID"].toString()!=QLatin1String(ZTGUI_PLUGIN_IID) || metadata["MetaData"].toObject()["apiVersion"].toString()!=QLatin1String(ApiVersion))
                error="Inner plugin metadata mismatch";
            if(pkg.info.value("formatVersion").toInt()==3) for(const auto &field:{"dependencies","provides"})
                if(metadata["MetaData"].toObject()[field]!=pkg.info[field]) error="Inner plugin service metadata mismatch";
            if(error.isEmpty() && !cancelled()) {
                // Runtime activation mutates process-wide module/search-path state.
                static QMutex runtimeMutex;
                const QMutexLocker lock(&runtimeMutex);
                if(!cancelled()) error=package::activateRuntime(pkg,loaded.cache->path());
            }
            if(error.isEmpty() && !cancelled() && !loaded.library->load()) error=loaded.library->errorString();
        } catch(const std::exception &e) { error=QString::fromUtf8(e.what()); }
        catch(...) { error="Plugin loading threw an exception"; }
        if(loaded.library) loaded.library->moveToThread(guiThread);
        return staged;
    }));
    m_loads.push_back(std::move(request));
    emit stateChanged(); return {};
}

void PluginLoader::finishLoad(QFutureWatcher<std::shared_ptr<StagedPlugin>> *watcher) {
    const auto request=std::find_if(m_loads.begin(),m_loads.end(),[watcher](const Loading &load) { return load.watcher.get()==watcher; });
    if(request==m_loads.end()) return;
    if(m_busy || (request->registry && request->registry->m_busy)) {
        QTimer::singleShot(10,this,[this,watcher] { finishLoad(watcher); }); return;
    }
    const QScopedValueRollback<bool> guard(m_busy,true);
    const auto staged=watcher->result();
    auto loaded=std::move(staged->plugin); const auto &pkg=staged->package;
    auto error=staged->error; const auto id=pkg.info.value("id").toString(); const auto key=id.toLower();
    auto registryTarget=request->registry; const bool enable=request->enable;
    QString catalogKey=request->key;
    if(m_stopping || request->cancellation.token().isCancellationRequested() || !registryTarget)
        error=QStringLiteral("插件加载已取消");
    const bool worker=pkg.info.value("execution").toObject()["kind"]=="worker-process";
    if(error.isEmpty() && !worker) {
        for(const auto &other:m_libraries) if(other.id.compare(id,Qt::CaseInsensitive)==0) error="Plugin is loaded or still stopping";
        for(const auto &registered:registryTarget->plugins()) {
            if(registered.id.compare(id,Qt::CaseInsensitive)==0) error="Plugin ID already registered";
            if(registered.name.compare(pkg.info.value("name").toString(),Qt::CaseInsensitive)==0) error="Plugin data directory/name already used";
        }
    }
    if(!key.isEmpty() && catalogKey!=key && !m_catalog.contains(key)) {
        m_catalog.insert(key,m_catalog.take(catalogKey)); catalogKey=key;
    }
    auto &row=m_catalog[catalogKey];
    if(!pkg.info.isEmpty()) {
        row["id"]=id; row["name"]=pkg.info.value("name").toString(); row["version"]=pkg.info.value("version").toString();
        row["api"]=pkg.info.value("apiVersion").toString();
    }
    m_loads.erase(request);
    if(error.isEmpty() && staged->disabled) {
        row["loading"]=false; row["message"]="Disabled"; emit stateChanged(); return;
    }
    if(error.isEmpty() && !worker) {
        // Registry, plugin QObjects and lifecycle hooks remain on the GUI thread.
        auto &registry=*registryTarget;
        try {
            auto *instance=loaded.library->instance(); auto *entry=qobject_cast<PluginInterface *>(instance);
            if(!entry) error=loaded.library->errorString();
            else {
                auto definition=entry->definition(); const auto &info=definition.info;
                if(info.id!=id || info.name!=pkg.info.value("name").toString() || info.version!=pkg.info.value("version").toString()) error="Package and plugin definition differ";
                else {
                    loaded.lifecycle=qobject_cast<PluginLifecycleInterface *>(instance);
                    loaded.stopInterface=qobject_cast<PluginStopInterface *>(instance);
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
        row["loading"]=false; row["loadable"]=staged->loadable; row["message"]=error;
        emit stateChanged(); emit loadFinished(id,false,error); return;
    }
    auto &registry=*registryTarget;
    loaded.id=id; loaded.registry=&registry; loaded.instance=++m_instanceSerial;
    loaded.services=std::make_unique<util::HostServicesImpl>(pkg.info.value("name").toString(),QString(),id);
    for(const auto &value:pkg.info.value("dependencies").toArray()) {
        const auto dependency=value.toObject(); const auto provider=dependency["pluginId"].toString().toCaseFolded();
        for(const auto &p:m_libraries) if(p.id.toCaseFolded()==provider && p.ready && !p.unloading && p.worker)
            loaded.services->serviceClient.providers.insert(provider,{p.worker.get(),dependency["services"].toArray()});
    }
    if(worker) {
        api::PluginDefinition definition; definition.info={id,pkg.info.value("name").toString(),pkg.info.value("version").toString()};
        const auto result=registry.registerPlugin(std::move(definition),nullptr,true);
        if(!result.ok()) { row["loading"]=false; row["message"]=result.message; emit stateChanged(); emit loadFinished(id,false,result.message); return; }
        loaded.worker=std::make_unique<ServiceBroker>();
        auto *process=loaded.worker.get();
        connect(process,&ServiceBroker::becameReady,this,[this,id,registryTarget] {
            for(auto &p:m_libraries) if(p.id==id) {
                if(p.unloading || m_stopping) return;
                p.ready=true; registryTarget->setPreparationState(id,PreparationState::Ready);
                auto &row=m_catalog[id.toCaseFolded()]; row["loading"]=false; row["message"]="Ready"; row["pid"]=p.worker->pid(); row["generation"]=QString::number(p.worker->generation());
                emit stateChanged(); emit loadFinished(id,true,{}); break;
            }
        });
        connect(process,&ServiceBroker::failed,this,[this,id,registryTarget](const QString &error) {
            for(auto &p:m_libraries) if(p.id==id) { p.ready=false; p.keep=false; p.unloading=true; }
            if(registryTarget) registryTarget->setPreparationState(id,PreparationState::Failed,error);
            auto &row=m_catalog[id.toCaseFolded()]; row["loading"]=false; row["message"]=error;
            // Invalidate dependents without restoring capture on a new instance.
            const auto impact=unloadImpact(id);
            for(auto &p:m_libraries) if(impact.contains(p.id)) { p.keep=false; p.unloading=true; p.cancellation.requestCancellation(); if(p.registry) p.registry->setPreparationState(p.id,PreparationState::Failed,"Provider failed: "+error); }
            m_shutdownCheck.start(); emit stateChanged(); emit loadFinished(id,false,error);
        });
        connect(process,&ServiceBroker::changed,this,[this,id,process] {
            if(process->preparing()) {
                auto &row=m_catalog[id.toCaseFolded()]; row["loading"]=true; row["message"]="Preparing service";
                emit stateChanged();
            }
            m_shutdownCheck.start();
        });
        m_libraries.push_back(std::move(loaded));
        auto &p=m_libraries.back(); row["loaded"]=true; row["message"]="Starting worker";
        process->start(p.cache->filePath(pkg.info.value("entry").toString()),p.cache->path(),pkg.info);
        emit stateChanged(); return;
    }
    m_libraries.push_back(std::move(loaded)); auto &plugin=m_libraries.back();
    row["loaded"]=true; row["message"]=plugin.lifecycle?"Preparing":"Ready";
    CreateTask task; QString failure;
    try { const QScopedValueRollback<bool> creating(registry.m_busy,true);
        if(plugin.lifecycle) task=plugin.lifecycle->onCreate(*plugin.services,plugin.cancellation.token());
    } catch(const std::exception &e) { failure=QString::fromUtf8(e.what()); }
    catch(...) { failure="onCreate threw an exception"; }
    if(failure.isEmpty()&&!task) {
        registry.setPreparationState(id,PreparationState::Ready); row["loading"]=false; row["message"]="Ready";
        emit loadFinished(id,true,QString());
    }
    else prepare(std::move(task),failure,id,pkg.info.value("name").toString(),registry,plugin.cancellation.token());
    emit stateChanged();
}

void PluginLoader::prepare(CreateTask task, const QString &failure, const QString &pluginId,
                          const QString &pluginName, PluginRegistry &registry, util::CancellationToken cancellation)
{
    registry.setPreparationState(pluginId, PreparationState::Preparing, QStringLiteral("插件准备中…"));
    if (!task) {
        QTimer::singleShot(0,this,[this,pluginId] { for(auto &p:m_libraries) if(p.id==pluginId) { p.unloading=true; p.services->stopRequests(); m_shutdownCheck.start(); } });
        registry.setPreparationState(pluginId, PreparationState::Failed, failure);
        auto &row=m_catalog[pluginId.toLower()]; row["loading"]=false; row["message"]=failure;
        emit stateChanged(); emit loadFinished(pluginId,false,failure);
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
    auto &row=m_catalog[pluginId.toLower()]; row["loading"]=false; row["message"]=result.success?QString("Ready"):QString(result.message.constData(),result.message.size()); emit stateChanged();
    emit loadFinished(pluginId,result.success,result.message);
    if(!result.success) {
        for(auto &p:m_libraries) if(p.id==pluginId) { p.unloading=true; p.services->stopRequests(); }
        m_shutdownCheck.start();
    }
}

bool PluginLoader::release(LoadedPlugin &plugin)
{
    if(plugin.releaseFailed) return false;
    if(plugin.worker) {
        plugin.worker->stop();
        if(!plugin.worker->exited() || !plugin.worker->idle()) return false;
        plugin.worker.reset(); plugin.services.reset(); plugin.cacheLock.reset(); package::discardCache(plugin.cache); return true;
    }
    plugin.services->stopRequests();
    plugin.services->serviceClient.closeAll();
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
    for(auto &p:m_libraries) if(p.id.compare(pluginId,Qt::CaseInsensitive)==0&&p.registry==&registry) {
        if((p.unloading || p.releaseFailed) && !p.stopFailed) return {Error::Busy,"Plugin is stopping or could not unload"};
        const auto affected=unloadImpact(p.id);
        QStringList users;
        for(const auto &id:affected) if(id.compare(p.id,Qt::CaseInsensitive)!=0) {
            const auto name=m_catalog.value(id.toCaseFolded()).value("name").toString();
            users.append(name.isEmpty() || name==id?id:name+" ("+id+")");
        }
        if(!users.isEmpty()) return {Error::Busy,QStringLiteral("当前插件仍被以下插件依赖，请先卸载这些插件：\n%1").arg(users.join(QStringLiteral("、")))};
        if(p.stopFailed) { p.releaseFailed=false; p.stopFailed=false; p.stopStarted=false; p.releaseError.clear(); }
        if(const auto result=registry.checkMutation();!result.ok()) return result;
        for(auto &plugin:m_libraries) if(affected.contains(plugin.id)) {
            plugin.keep=false; plugin.unloading=true; plugin.cancellation.requestCancellation();
            plugin.services->serviceClient.beginStop();
            if(plugin.registry) { if(plugin.registry->m_active.plugin.id==plugin.id) plugin.registry->activateMenu(ZeroTierMenuId); plugin.registry->setPreparationState(plugin.id,PreparationState::Stopping,"Stopping"); }
            auto &row=m_catalog[plugin.id.toCaseFolded()]; row["stopping"]=true; row["message"]="Stopping";
            QSettings().remove("plugins/retain/"+plugin.id.toCaseFolded());
        }
        for(auto it=m_transactions.begin();it!=m_transactions.end();) {
            if(it->order.contains(pluginId.toCaseFolded())) {
                for(auto &load:m_loads) if(it->order.contains(load.key)) load.cancellation.requestCancellation();
                m_catalog[it->target]["loading"]=false; it=m_transactions.erase(it);
            } else ++it;
        }
        QSettings().setValue("plugins/disabled/"+pluginId.toLower(),true);
        m_shutdownCheck.start(); emit stateChanged(); return {};
    }
    bool cancelled=false;
    for(auto &load:m_loads) if(load.key.compare(pluginId,Qt::CaseInsensitive)==0 || m_catalog.value(load.key).value("path")==pluginId) { load.cancellation.requestCancellation(); cancelled=true; }
    for(auto it=m_transactions.begin();it!=m_transactions.end();) {
        if(it->target.compare(pluginId,Qt::CaseInsensitive)==0 || it->packages.value(it->target).path==pluginId) {
            for(auto &load:m_loads) if(it->order.contains(load.key)) {
                bool shared=false; for(const auto &other:m_transactions) if(&other!=&*it && other.order.contains(load.key)) shared=true;
                if(!shared) load.cancellation.requestCancellation();
            }
            cancelled=true; it=m_transactions.erase(it);
        } else ++it;
    }
    QString catalogKey=pluginId.toCaseFolded();
    for(auto row=m_catalog.cbegin();row!=m_catalog.cend();++row) if((row.value()["id"].toString().compare(pluginId,Qt::CaseInsensitive)==0 || row.value()["path"]==pluginId) && row.value()["loading"].toBool()) {
        m_cancelledPaths.insert(row.value()["path"].toString()); catalogKey=row.key(); cancelled=true;
    }
    if(cancelled) {
        auto &row=m_catalog[catalogKey]; row["loading"]=false; row["message"]="Plugin loading cancelled";
        bool completionPending=false;
        for(const auto &load:m_loads) if(load.key==catalogKey || m_catalog.value(load.key).value("path")==pluginId) completionPending=true;
        for(const auto &d:m_discoveries) if(d.roots.contains(row["path"].toString())) completionPending=true;
        if(!row["id"].toString().isEmpty()) QSettings().setValue("plugins/disabled/"+row["id"].toString().toCaseFolded(),true);
        scheduleAdvance(); emit stateChanged();
        if(!completionPending) emit loadFinished(row["id"].toString(),false,"Plugin loading cancelled");
        return {};
    }
    return {Error::NotFound,"Plugin is not loaded"};
}
QStringList PluginLoader::unloadImpact(const QString &pluginId) const {
    QString target=pluginId;
    for(const auto &p:m_libraries) if(p.id.compare(pluginId,Qt::CaseInsensitive)==0) { target=p.id; break; }
    QStringList result{target};
    for(int n=0;n<result.size();++n) {
        const auto key=result[n].toCaseFolded();
        for(const auto &p:m_libraries)
            if(p.dependencies.contains(key) && !result.contains(p.id)) result.append(p.id);
        // A consumer reserved by a load transaction also keeps its provider alive.
        for(const auto &t:m_transactions) if(t.target!=key && t.order.contains(key)) {
            const auto id=m_catalog.value(t.target).value("id",t.target).toString();
            if(!result.contains(id)) result.append(id);
        }
    }
    QStringList order;
    for(auto p=m_libraries.crbegin();p!=m_libraries.crend();++p) if(result.contains(p->id)) order.append(p->id);
    for(const auto &id:result) if(!order.contains(id)) order.append(id);
    return order;
}
void PluginLoader::stopPlugin(LoadedPlugin &p) {
    if(p.stopStarted) return;
    p.stopStarted=true;
    p.services->serviceClient.beginStop();
    if(!p.stopInterface) { p.stopDone=true; p.services->stopRequests(); p.services->serviceClient.closeAll(); return; }
    const auto id=p.id; const auto instance=p.instance; const auto attempt=++p.stopAttempt;
    const auto complete=[this,id,instance,attempt](CreateResult result) {
        QMetaObject::invokeMethod(this,[this,id,instance,attempt,result=std::move(result)] {
            for(auto &plugin:m_libraries) if(plugin.id==id && plugin.instance==instance && plugin.stopAttempt==attempt && !plugin.stopDone && !plugin.releaseFailed) {
                if(!result.success) {
                    plugin.releaseFailed=true; plugin.stopFailed=true;
                    plugin.releaseError=result.message.isEmpty()?"Plugin stop failed":result.message;
                    m_catalog[id.toCaseFolded()]["message"]=plugin.releaseError;
                    m_catalog[id.toCaseFolded()]["stopping"]=false;
                    emit unloadFinished(id,false,plugin.releaseError);
                } else {
                    plugin.stopDone=true; plugin.services->stopRequests(); plugin.services->serviceClient.closeAll();
                }
                emit stateChanged(); break;
            }
        },Qt::QueuedConnection);
    };
    try { p.stopInterface->requestStop(this,complete); }
    catch(const std::exception &e) { complete({false,QString::fromUtf8(e.what())}); }
    catch(...) { complete({false,"Plugin stop threw an exception"}); }
}
void PluginLoader::drainUnloads() {
    if(m_busy||!m_preparationPool.waitForDone(0)) return;
    for(const auto &p:m_libraries) if(p.worker && p.worker->dispatching()) return;
    const QScopedValueRollback<bool> guard(m_busy,true);
    for(auto i=m_libraries.begin();i!=m_libraries.end();) {
        if(!i->unloading||i->releaseFailed) { ++i; continue; }
        i->services->serviceClient.beginStop();
        if(hasDependent(i->id)) { m_catalog[i->id.toCaseFolded()]["message"]="Waiting for dependents"; ++i; continue; }
        i->services->stopRequests();
        bool pending=false;
        for(auto &task:m_preparations) if(task.id==i->id&&!task.watcher->isFinished()) pending=true;
        if(pending || i->services->hasCallbacks() || i->services->serviceClient.hasCallbacks()) { ++i; continue; }
        if(i->worker) {
            if(!i->worker->idle()) { ++i; continue; }
            i->worker->stop(); if(!i->worker->exited()) { ++i; continue; }
            if(i->worker->forced()) {
                auto &row=m_catalog[i->id.toCaseFolded()]; row["forcedTermination"]=true;
                const auto cause=i->worker->failure();
                row["message"]=cause.isEmpty()?QString("Worker terminated after stop timeout"):cause+"; worker terminated after stop timeout";
            }
        } else {
            stopPlugin(*i);
            if(!i->stopDone || i->releaseFailed || !i->services->serviceClient.drained()) { ++i; continue; }
        }
        for(auto t=m_preparations.begin();t!=m_preparations.end();) {
            if(t->id==i->id) { disconnect(t->watcher.get(),nullptr,this,nullptr); t=m_preparations.erase(t); } else ++t;
        }
        if(i->registry) {
            const auto result=i->registry->unregisterPlugin(i->id);
            if(result.error==Error::Busy) { ++i; continue; }
        }
        const auto id=i->id; const bool ok=release(*i);
        auto &row=m_catalog[id.toLower()]; row["loaded"]=!ok; row["loading"]=false; row["stopping"]=false;
        if(ok) {
            row["disabled"]=QSettings().value("plugins/disabled/"+id.toCaseFolded(),false).toBool();
            if(!row["forcedTermination"].toBool() && (row["message"].toString()=="Stopping" || m_stopping)) row["message"]=row["disabled"].toBool()?"Disabled":"Not loaded";
            i=m_libraries.erase(i);
        }
        else { row["message"]=i->releaseError; ++i; }
        emit stateChanged(); emit unloadFinished(id,ok,row["message"].toString());
    }
    if(!m_stopping&&std::none_of(m_libraries.begin(),m_libraries.end(),[](const LoadedPlugin &p) { return p.unloading&&!p.releaseFailed; })) m_shutdownCheck.stop();
}

QStringList PluginLoader::pendingInitializations() const
{
    QStringList pending;
    for(const auto &d:m_discoveries) for(const auto &root:d.roots) pending.append(QFileInfo(root).completeBaseName());
    for(const auto &load:m_loads) pending.append(m_catalog.value(load.key).value("name").toString());
    for(const auto &p:m_libraries) if(p.unloading) pending.append(p.releaseFailed?p.id+": "+p.releaseError:p.id);
    for (const auto &preparation : m_preparations) {
        if (!preparation.watcher->isFinished()) pending.append(preparation.name);
    }
    pending.removeDuplicates();
    return pending;
}

void PluginLoader::requestShutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_stopping) {
        for(auto &p:m_libraries) if(p.stopFailed) { p.stopFailed=false; p.releaseFailed=false; p.stopStarted=false; p.releaseError.clear(); }
        if(!m_shutdownComplete) m_shutdownCheck.start();
        return;
    }
    m_stopping = true;
    m_transactions.clear();
    for(auto &load:m_loads) load.cancellation.requestCancellation();
    for (auto &plugin : m_libraries) { plugin.cancellation.requestCancellation(); plugin.unloading=true; plugin.services->serviceClient.beginStop(); }
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
    if (!pending.isEmpty() || !m_loadingPool.waitForDone(0) || !m_preparationPool.waitForDone(0)) return;
    drainUnloads();
    if(!m_libraries.empty()) return;
    m_shutdownCheck.stop();
    m_shutdownComplete = true;
    emit shutdownFinished();
}
}
