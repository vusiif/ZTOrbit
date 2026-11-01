#include "AppController.h"
#include "MemberPing.h"
#include "PluginPackage.h"
#include "AppLog.h"
#include "NetworkBundle.h"
#include "NetworkMembers.h"
#include "PluginImport.h"
#include "SingleInstance.h"
#include "storage/StorageMaintenance.h"
#include <ztprotocol/NetworkAddress.h>
#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonParseError>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>
#include <QBuffer>
#include <QCryptographicHash>
#include <algorithm>
#include <limits>
#ifdef Q_OS_WIN
#include <windows.h>
#include <commctrl.h>
#endif

namespace vusiif::zto::ui {
namespace {
#ifdef Q_OS_WIN
LRESULT CALLBACK captionProcedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    auto *controller = reinterpret_cast<AppController *>(data);
    if (message == WM_SYSCOMMAND && (wParam & 0xfff0) == SC_CLOSE && lParam != 0 && !controller->closing()) {
        controller->hideToTray();
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(handle, captionProcedure, id);
    return DefSubclassProc(handle, message, wParam, lParam);
}
#endif
QVariantMap row(const QString &id, const QString &name, const QStringList &cells, const QJsonObject &details)
{
    return {{"rowId", id}, {"name", name}, {"cells", cells}, {"details", details.toVariantMap()}};
}
QStringList addresses(const QJsonArray &values)
{
    QStringList result;
    for (const auto &value : values) {
        const auto ip = value.toString().section('/', 0, 0);
        if (!ip.isEmpty() && !result.contains(ip)) result.append(ip);
    }
    return result;
}
QString normalizeMoonId(const QString &id)
{
    return QRegularExpression("^[0-9a-fA-F]{1,16}$").match(id).hasMatch() ? id.toLower().rightJustified(16, '0') : id;
}
QString sha256Hex(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
}
AppController::AppController(QObject *parent, const QString &pluginDirectory, SystemOperations *operations,
                             const storage::StorageContext &storage)
    : QObject(parent), m_storage(storage), m_settings(m_storage.settingsFile()),
      m_operations(operations ? operations : new SystemOperations(this)),
      m_infoPoller(new zerotier::JsonPoller(this, "info", true)),
      m_networkPoller(new zerotier::JsonPoller(this, "listnetworks")),
      m_peerPoller(new zerotier::JsonPoller(this, "listpeers")),
      m_moonPoller(new zerotier::JsonPoller(this, "listmoons")),
      m_pluginDirectory(pluginDirectory.isEmpty() ? QCoreApplication::applicationDirPath() + "/plugins" : pluginDirectory)
{
    QString storageError;
    if (m_storage.isValid()) m_storage.prepare(storageError);
    else storageError = m_storage.error();
    if (m_storage.isValid()) {
        package::CacheContext cacheContext;
        cacheContext.root = m_storage.pluginPackageCacheRoot();
        cacheContext.userKey = m_storage.userKeyValue();
        cacheContext.runId = m_storage.runId();
        cacheContext.installKey = m_storage.installKeyValue();
        cacheContext.legacy = false;
        const auto cacheError = package::prepareCacheSession(cacheContext);
        if (!cacheError.isEmpty() && storageError.isEmpty()) storageError = cacheError;
        package::cleanStaleCache(cacheContext);
    }
    QString errorMessage = storageError;
    // A failed copy of the host INI must not be occupied by defaults: every
    // persistent host write (C++ setters, plugin enable state and QML window /
    // column state through setHostSetting) is refused until the retry settles.
    const auto settingsProtection = storage::SettingsStore::guardStorageMigration(m_settings, m_storage);
    const auto migration = storage::SettingsStore::migrateLegacy(m_settings, m_storage.metadataDirectory(),
                                                                 m_storage.userKey());
    QStringList warnings = m_storage.warnings();
    if (!settingsProtection.isEmpty()) warnings.append(settingsProtection);
    for (const auto &warning : migration.warnings) warnings.append(warning);
    if (!migration.error.isEmpty()) errorMessage = migration.error;
    for (const auto &warning : warnings) util::log::write(QStringLiteral("存储：") + warning);
    if (!errorMessage.isEmpty()) util::log::write(QStringLiteral("存储错误：") + errorMessage);
    m_storageWarning = errorMessage.isEmpty() ? warnings.join(QLatin1Char('\n')) : errorMessage;
    m_loader.setSettings(&m_settings);
    m_loader.setStorage(m_storage);
    m_loader.setCacheWriteNotifier([this](const QString &path) {
        if (m_maintenance) m_maintenance->notifyCacheWrite(path);
    });
    // Authorized deletes recorded by an earlier run must complete before the
    // package scan can see the moved packages as installed.
    const auto recoveredDeletions = m_loader.recoverPendingDeletions(m_pluginDirectory);
    if (recoveredDeletions > 0)
        util::log::write(QStringLiteral("存储：已完成 %1 个待删插件事务。").arg(recoveredDeletions));
    const auto pendingIssues = m_loader.pendingDeletionIssues();
    if (!pendingIssues.isEmpty()) {
        for (const auto &issue : pendingIssues) util::log::write(QStringLiteral("存储：") + issue);
        const auto text = pendingIssues.join(QLatin1Char('\n'));
        m_storageWarning = m_storageWarning.isEmpty() ? text : m_storageWarning + QLatin1Char('\n') + text;
    }
    m_serviceStatus.known = false;
    connect(&m_loader,&plugins::PluginLoader::stateChanged,this,&AppController::menusChanged);
    connect(&m_loader,&plugins::PluginLoader::loadFinished,this,[this](const QString &id,bool success,const QString &message) {
        if(!success && !m_closing) {
            const auto detail=QStringLiteral("插件 %1 加载失败：%2").arg(id,message);
            feedback(detail); emit pluginNoticeRequested(QStringLiteral("无法加载插件"),detail);
        }
    });
    connect(&m_registry, &plugins::PluginRegistry::menusChanged, this, [this] {
        bool exists = m_activeMenu == QLatin1String(api::ZeroTierMenuId) || m_activeMenu == QLatin1String(api::SettingsMenuId);
        for (const auto &menu : m_registry.menus()) exists |= menu.id == m_activeMenu;
        if (exists) emit menusChanged();
        else { m_activeMenu = QString::fromLatin1(api::ZeroTierMenuId); emit activeMenuChanged(); emit menusChanged(); updatePolling(); }
    });
    connect(m_infoPoller, &zerotier::JsonPoller::updated, this, [this](const QJsonDocument &doc) {
        m_info = doc.object().toVariantMap(); emit stateChanged();
    });
    connect(m_infoPoller, &zerotier::JsonPoller::availabilityChanged, this, [this](bool available, const QString &) {
        m_apiAvailable = available; emit stateChanged();
    });
    connect(m_networkPoller, &zerotier::JsonPoller::availabilityChanged, this, [this](bool available, const QString &message) {
        m_networkStatus = available ? QString() : message; emit stateChanged();
    });
    connect(m_peerPoller, &zerotier::JsonPoller::availabilityChanged, this, [this](bool available, const QString &message) {
        m_peerStatus = available ? QString() : message; emit stateChanged();
    });
    connect(m_networkPoller, &zerotier::JsonPoller::updated, this, [this](const QJsonDocument &doc) {
        QList<QVariantMap> rows;
        for (const auto &value : doc.array()) {
            const auto object = value.toObject();
            const auto id = object["nwid"].toString(object["id"].toString());
            const auto ips = addresses(object["assignedAddresses"].toArray());
            rows.append(row(id, localName("networks", id, object["name"].toString()),
                {id, zerotier::translate(object["status"].toString()), ips.isEmpty() ? QStringLiteral("—") : ips.join(", ")}, object));
        }
        m_networks.updateRows(rows);
        for (const auto &id : m_memberRequests.keys()) if (m_networks.record(id).isEmpty()) delete m_memberRequests.take(id).data();
        for(const auto &id:m_memberProbeContexts.keys()) if(m_networks.record(id).isEmpty()) delete m_memberProbeContexts.take(id).data();
    });
    connect(m_peerPoller, &zerotier::JsonPoller::updated, this, [this](const QJsonDocument &doc) {
        QList<QVariantMap> rows;
        for (const auto &value : doc.array()) {
            const auto object = value.toObject();
            const auto role = object["role"].toString();
            const int latency = object["latency"].toInt(-1);
            if (role == "LEAF" && latency < 0) continue;
            bool direct = false;
            QStringList ips;
            for (const auto &path : object["paths"].toArray()) {
                const auto connection = path.toObject();
                direct |= connection["active"].toBool();
                auto ip = connection["address"].toString().section('/', 0, 0);
                if (ip.startsWith('[') && ip.endsWith(']')) ip = ip.mid(1, ip.size() - 2);
                if (!ip.isEmpty() && !ips.contains(ip)) ips.append(ip);
            }
            const auto id = object["address"].toString();
            rows.append(row(id, localName("peers", id), {zerotier::translate(role),
                latency < 0 ? QStringLiteral("不可达") : zerotier::translate(direct ? "DIRECT" : "RELAY"),
                ips.isEmpty() ? QStringLiteral("—") : ips.join(", "), latency < 0 ? QStringLiteral("—") : QString::number(latency)}, object));
        }
        const auto rank = [](const QVariantMap &record) {
            const auto cells = record["cells"].toStringList();
            return (record["details"].toMap()["role"].toString() == "PLANET" ? 10 : 0)
                + (cells.value(1) == QStringLiteral("直链") ? 0 : cells.value(1) == QStringLiteral("中转") ? 1 : 2);
        };
        std::stable_sort(rows.begin(), rows.end(), [&](const auto &a, const auto &b) { return rank(a) < rank(b); });
        m_peers.updateRows(rows);
    });
    connect(m_moonPoller, &zerotier::JsonPoller::updated, this, [this](const QJsonDocument &doc) {
        m_moonMetadata = doc; refreshMoonFiles();
    });
    m_statusTimer.setInterval(1000);
    connect(&m_statusTimer, &QTimer::timeout, this, &AppController::updateService);
    m_memberTimer.setInterval(5000);
    connect(&m_memberTimer, &QTimer::timeout, this, &AppController::refreshMembers);
    connect(m_operations, &SystemOperations::output, this, [this](const QString &text) { feedback(text.trimmed()); });
    connect(&m_loader, &plugins::PluginLoader::shutdownProgress, this, [this](const QStringList &pending) {
        m_shutdownMessage = QStringLiteral("正在退出，正在停止插件初始化…");
        if (!pending.isEmpty()) m_shutdownMessage += pending.join(QStringLiteral("、"));
        emit stateChanged();
    });
    connect(&m_loader, &plugins::PluginLoader::shutdownFinished, this, [this] {
        m_shutdownReady = true;
        if (!m_busy) emit exitFinished();
    });
#ifndef ZTO_TESTING
    m_serviceMonitor = zerotier::createServiceMonitor(this);
    for (const auto &message : m_loader.scan(m_pluginDirectory, m_registry)) util::log::write(message);
#else
    if (!pluginDirectory.isEmpty()) m_loader.scan(m_pluginDirectory, m_registry);
#endif
    updateService(); refreshMoonFiles();
    m_localConfigPath = zerotier::localConfigPath();
    loadLocalConfig(true);
    refreshStorageUsage();
    m_maintenance = std::make_unique<storage::StorageMaintenanceScheduler>(m_storage, this);
    connect(m_maintenance.get(), &storage::StorageMaintenanceScheduler::cleanupFinished, this,
            [this](const storage::CleanupReport &report) {
        m_storageMaintenance = QVariantMap{{"releasedBytes", double(report.releasedBytes)},
            {"released", report.released.size()}, {"skipped", report.skipped.size()},
            {"failed", report.failed.size()}, {"error", report.error},
            {"lastRun", m_maintenance ? m_maintenance->lastRun().toString(Qt::ISODate) : QString()}};
        if (!report.error.isEmpty())
            util::log::write(QStringLiteral("存储自动维护：%1").arg(report.error));
        // Update only the maintenance line; a full directory scan here would
        // block the GUI on every debounced cache write.
        m_storageUsage["maintenance"] = maintenanceText();
        emit storageUsageChanged();
    });
    // Rotation and cache-write notifications only schedule work; the cleanup
    // itself runs on a worker thread and never blocks the GUI.
    util::log::setRotationCallback([scheduler = QPointer<storage::StorageMaintenanceScheduler>(m_maintenance.get())] {
        if (scheduler) scheduler->notifyLogRotation();
    });
#ifndef ZTO_TESTING
    // GUI regressions use these controllers with temporary storage; automatic
    // maintenance is covered by dedicated scheduler tests instead of running
    // background filesystem scans during unrelated timing-sensitive cases.
    m_maintenance->start();
#endif
}
AppController::~AppController()
{
    util::log::setRotationCallback({});
    m_maintenance.reset();
#ifdef Q_OS_WIN
    if (m_windowId && IsWindow(reinterpret_cast<HWND>(m_windowId)))
        RemoveWindowSubclass(reinterpret_cast<HWND>(m_windowId), captionProcedure, reinterpret_cast<UINT_PTR>(this));
#endif
}
QVariantMap AppController::service() const
{
    QString warning;
    if (!m_serviceStatus.known && !m_serviceQueryPending) warning = QStringLiteral("无法读取服务状态，请检查管理员权限。");
    else if (m_apiAvailable && m_info["tcpFallbackActive"].toBool() && !m_localConfig.forceTcpRelay)
        warning = QStringLiteral("服务使用 TCP 中转，请检查网络连接。");
    return {{"exists", m_serviceStatus.exists}, {"running", m_serviceStatus.running}, {"known", m_serviceStatus.known},
            {"available", m_apiAvailable}, {"warning", warning},
            {"action", !m_serviceStatus.exists ? QStringLiteral("安装") : m_serviceStatus.running ? QStringLiteral("停止") : QStringLiteral("启动")}};
}
void AppController::updateService()
{
    if (m_busy || m_closing || m_serviceQueryPending) return;
    if (m_moonsActive) refreshMoonFiles();
#ifdef Q_OS_WIN
    m_serviceQueryPending = true;
    auto *watcher = new QFutureWatcher<zerotier::ServiceStatus>(this);
    connect(watcher, &QFutureWatcher<zerotier::ServiceStatus>::finished, this, [this, watcher] {
        m_serviceQueryPending = false;
        m_serviceStatus = watcher->result(); watcher->deleteLater(); emit stateChanged();
    });
    watcher->setFuture(QtConcurrent::run([] { return zerotier::serviceStatus(); }));
#else
    // macOS status is already cached by an asynchronous launchctl process.
    m_serviceStatus = zerotier::serviceStatus(); emit stateChanged();
#endif
}
QString AppController::theme() const { return m_settings.value("appearance/theme", "light").toString() == "dark" ? "dark" : "light"; }
bool AppController::dark() const { return theme() == "dark"; }
void AppController::setTheme(const QString &value)
{
    if (value != "light" && value != "dark") return;
    m_settings.setValue("appearance/theme", value); emit themeChanged();
}
bool AppController::disableGpuAcceleration() const
{
    return m_settings.value("rendering/disableGpuAcceleration", true).toBool();
}
void AppController::setDisableGpuAcceleration(bool disabled)
{
    if (disableGpuAcceleration() == disabled) return;
    m_settings.setValue("rendering/disableGpuAcceleration", disabled);
    const bool showHint=!m_settings.value("rendering/restartHintShown",false).toBool();
    m_settings.setValue("rendering/restartHintShown",true);
    QString syncError;
    if (!m_settings.sync(syncError)) feedback(QStringLiteral("无法保存设置：%1").arg(syncError));
    emit disableGpuAccelerationChanged();
    if(showHint) emit pluginNoticeRequested(QStringLiteral("需要重启程序"),QStringLiteral("GPU 加速设置已保存，完全退出并重新启动程序后生效。"));
}
bool AppController::gpuAccelerationForcedOff() const
{
    return QCoreApplication::arguments().contains("--disable-gpu-acceleration");
}
void AppController::setHostSetting(const QString &key, const QVariant &value)
{
    // QML window, sidebar and column state share the host INI with the C++
    // setters, so the shared-root migration gate applies here as well.
    m_settings.setValue(key, value);
    QString error;
    if (m_settings.sync(error)) return;
    // Resizing the splitter may retry on every pixel; report the first failure
    // once instead of flooding the feedback bar.
    if (!m_settingsWriteReported) {
        m_settingsWriteReported = true;
        util::log::write(QStringLiteral("存储：无法保存设置 %1：%2").arg(key, error));
    }
}
QString AppController::version() const { return QCoreApplication::applicationVersion(); }
QString AppController::apiVersion() const { return QString::fromLatin1(api::ApiVersion); }
QString AppController::serverVersion() const { return QStringLiteral(ZTOHUB_VERSION); }
QString AppController::serverApiVersion() const { return QStringLiteral(ZTOHUB_API_VERSION); }
QString AppController::qtVersion() const { return QString::fromLatin1(qVersion()); }
QString AppController::storageMode() const
{
    switch (m_storage.mode()) {
    case storage::StorageContext::Mode::Personal: return QStringLiteral("personal");
    case storage::StorageContext::Mode::Ephemeral: return QStringLiteral("ephemeral");
    case storage::StorageContext::Mode::Shared: break;
    }
    return QStringLiteral("shared");
}
QString AppController::storageWarning() const
{
    return m_storageWarning;
}
QStringList AppController::installedPluginIds() const
{
    QSet<QString> ids;
    for (const auto &value : m_loader.catalog()) {
        const auto id = value.toMap()["id"].toString();
        if (!id.isEmpty()) ids.insert(id);
    }
    for (const auto &plugin : m_registry.plugins()) ids.insert(plugin.id);
    auto result = QStringList(ids.cbegin(), ids.cend());
    std::sort(result.begin(), result.end());
    return result;
}
QString AppController::maintenanceText() const
{
    if (m_storageMaintenance.isEmpty()) return {};
    const auto error = m_storageMaintenance.value("error").toString();
    const auto lastRun = m_storageMaintenance.value("lastRun").toString();
    if (!error.isEmpty())
        return QStringLiteral("自动维护失败（%1）：%2").arg(lastRun, error);
    return QStringLiteral("最近自动维护 %1：释放 %2 项（%3 MiB）。")
        .arg(lastRun).arg(m_storageMaintenance.value("released").toInt())
        .arg(QString::number(m_storageMaintenance.value("releasedBytes").toDouble() / (1024.0 * 1024.0), 'f', 1));
}
void AppController::refreshStorageUsage()
{
    requestStorageScan();
}

void AppController::requestStorageScan()
{
    if (m_closing) return;
    // A running scan or cleanup owns the filesystem view; coalesce requests.
    if (m_storageScanBusy || m_storageBusy) {
        m_storageScanQueued = true;
        return;
    }
    m_storageScanBusy = true;
    m_storageScanQueued = false;
    m_storageScanError.clear();
    emit storageUsageChanged();
    const auto context = m_storage;
    const auto installed = installedPluginIds();
    const bool uncertainPackages = m_loader.hasUncertainPackages();
    auto *watcher = new QFutureWatcher<StorageScanResult>(this);
    m_storageScanWatcher = watcher;
    connect(watcher, &QFutureWatcher<StorageScanResult>::finished, this,
            &AppController::handleStorageScanFinished);
    // Directory walks, retention previews and orphan classification never run
    // on the GUI thread.
    watcher->setFuture(QtConcurrent::run([context, installed, uncertainPackages] {
        StorageScanResult result;
        storage::StorageMaintenance maintenance(context);
        result.usage = maintenance.usage();
        result.preview = maintenance.previewRetention();
        result.orphans = maintenance.orphans(installed, uncertainPackages);
        result.uncertainPackages = uncertainPackages;
        return result;
    }));
}

void AppController::handleStorageScanFinished()
{
    auto *watcher = m_storageScanWatcher;
    if (!watcher) return;
    const auto result = watcher->result();
    watcher->deleteLater();
    m_storageScanWatcher = nullptr;
    m_storageScanBusy = false;
    m_storageScanComplete = true;
    const auto usage = result.usage;
    m_storageOrphans.clear();
    qint64 orphanBytes = 0;
    int deletable = 0;
    int manual = 0;
    QStringList deletableNames;
    QStringList manualNames;
    for (const auto &item : result.orphans) {
        m_storageOrphans.append(QVariantMap{{"id", item.id}, {"directory", item.directory},
                                            {"path", item.path}, {"bytes", double(item.bytes)},
                                            {"deletable", item.deletable}, {"reason", item.reason}});
        orphanBytes += item.bytes;
        if (item.deletable) {
            ++deletable;
            deletableNames.append(item.directory);
        } else {
            ++manual;
            manualNames.append(item.directory);
        }
    }
    QString orphansText;
    if (result.orphans.isEmpty()) {
        orphansText = QStringLiteral("无主数据：无");
    } else {
        orphansText = QStringLiteral("无主数据：%1 项（%2 MiB）；可删除 %3 项，人工处理 %4 项")
                          .arg(result.orphans.size())
                          .arg(QString::number(orphanBytes / (1024.0 * 1024.0), 'f', 1))
                          .arg(deletable)
                          .arg(manual);
        if (!deletableNames.isEmpty()) {
            const auto shown = deletableNames.mid(0, 6).join(QStringLiteral("、"));
            orphansText += QStringLiteral("\n可删除：%1%2").arg(shown,
                deletableNames.size() > 6 ? QStringLiteral("…") : QString());
        }
        if (!manualNames.isEmpty()) {
            const auto shown = manualNames.mid(0, 6).join(QStringLiteral("、"));
            orphansText += QStringLiteral("\n需人工确认：%1%2").arg(shown,
                manualNames.size() > 6 ? QStringLiteral("…") : QString());
        }
    }
    if (result.uncertainPackages)
        orphansText += QStringLiteral("\n存在无法解析或未通过认证的安装包，相关数据归属不确定，已保守保留。");
    QString pendingText;
    const auto pending = m_loader.pendingDeletions();
    if (!pending.isEmpty())
        pendingText = QStringLiteral("有 %1 个已确认的插件删除事务尚未完成，重启程序会继续处理。").arg(pending.size());
    m_storageScanError = result.preview.error;
    m_storageUsage = {{"root", usage.root}, {"settings", usage.settings}, {"logs", usage.logs},
                      {"pluginData", usage.pluginData}, {"cache", usage.cache}, {"mode", usage.mode},
                      {"logsBytes", double(usage.logsBytes)}, {"pluginDataBytes", double(usage.pluginDataBytes)},
                      {"cacheBytes", double(usage.cacheBytes)}, {"logFiles", usage.logFiles},
                      {"previewLogFiles", result.preview.logFiles},
                      {"previewLogBytes", double(result.preview.logBytes)},
                      {"previewCacheFiles", result.preview.cacheFiles},
                      {"previewCacheBytes", double(result.preview.cacheBytes)},
                      {"previewText", [&] {
                           if (result.preview.empty()) return QStringLiteral("待清理：暂无按保留策略可释放的项目。");
                           return QStringLiteral("待清理：历史日志 %1 个（%2 MiB）；闲置缓存 %3 个（%4 MiB）")
                               .arg(result.preview.logFiles)
                               .arg(QString::number(result.preview.logBytes / (1024.0 * 1024.0), 'f', 1))
                               .arg(result.preview.cacheFiles)
                               .arg(QString::number(result.preview.cacheBytes / (1024.0 * 1024.0), 'f', 1));
                       }()},
                      {"orphansText", orphansText}, {"orphanCount", result.orphans.size()},
                      {"orphanBytes", double(orphanBytes)},
                      {"uncertainPackages", result.uncertainPackages},
                      {"scanError", m_storageScanError},
                      {"scannedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                      {"maintenance", maintenanceText()}, {"pendingDeletions", pendingText},
                      {"warning", storageWarning()}};
    emit storageUsageChanged();
    emit storageScanFinished();
    if (m_pendingStorageRequest.valid) {
        const auto request = m_pendingStorageRequest;
        m_pendingStorageRequest = {};
        m_storageScanQueued = false;
        if (request.confirmed) runStorageCleanup(request.category, request.ids);
        else cleanupStorage(request.category);
        return;
    }
    if (m_storageScanQueued) {
        m_storageScanQueued = false;
        requestStorageScan();
    }
}

QVariantList AppController::storageOrphans() const
{
    return m_storageOrphans;
}

void AppController::cleanupStorage(const QString &category)
{
    if (m_closing || m_storageBusy) return;
    if (m_storageScanBusy || !m_storageScanComplete) {
        m_pendingStorageRequest = {true, category, {}, false};
        requestStorageScan();
        return;
    }
    if (category == QLatin1String("orphans")) {
        QStringList ids;
        QStringList names;
        for (const auto &value : m_storageOrphans) {
            const auto row = value.toMap();
            if (!row["deletable"].toBool()) continue;
            ids.append(row["id"].toString());
            names.append(row["directory"].toString());
        }
        if (ids.isEmpty()) {
            feedback(QStringLiteral("没有可自动清理的无主插件数据；未登记归属的旧目录需要人工确认。"));
            return;
        }
        ask(QStringLiteral("清理无主插件数据"),
            QStringLiteral("将永久删除以下已无安装包使用的插件数据：\n%1\n\n包含插件配置和备注，无法撤销。").arg(names.join(QStringLiteral("、"))),
            QStringLiteral("永久删除"), [this, ids] { runStorageCleanup(QStringLiteral("orphans"), ids); });
        return;
    }
    if (category != QLatin1String("logs") && category != QLatin1String("cache")) return;
    const bool logs = category == QLatin1String("logs");
    const int count = m_storageUsage.value(logs ? QStringLiteral("previewLogFiles")
                                                : QStringLiteral("previewCacheFiles")).toInt();
    const auto bytes = m_storageUsage.value(logs ? QStringLiteral("previewLogBytes")
                                                 : QStringLiteral("previewCacheBytes")).toDouble();
    if (count <= 0) {
        feedback(QStringLiteral("没有需要自动清理的%1。")
                     .arg(logs ? QStringLiteral("历史日志") : QStringLiteral("闲置缓存")));
        return;
    }
    ask(QStringLiteral("存储清理"),
        QStringLiteral("将删除 %1 个%2文件，释放约 %3 MiB。\n当前进程活动日志和正在使用的文件会被跳过。")
            .arg(count)
            .arg(logs ? QStringLiteral("历史日志") : QStringLiteral("闲置缓存"))
            .arg(QString::number(bytes / (1024.0 * 1024.0), 'f', 1)),
        QStringLiteral("清理"), [this, category] { runStorageCleanup(category, {}); });
}

void AppController::runStorageCleanup(const QString &category, const QStringList &ids)
{
    if (m_closing || m_storageBusy) return;
    if (category == QLatin1String("orphans") && m_loader.hasUncertainPackages()) {
        const auto message = QStringLiteral("存在无法解析或未通过认证的安装包，数据归属不确定，已取消清理。");
        feedback(message);
        emit pluginNoticeRequested(QStringLiteral("存储清理"), message);
        requestStorageScan();
        return;
    }
    // Scans and cleanups never overlap; a confirmed cleanup resumes after the
    // running scan so the GUI keeps its current view until then.
    if (m_storageScanBusy) {
        m_pendingStorageRequest = {true, category, ids, true};
        return;
    }
    m_storageBusy = true;
    m_storageScanComplete = false;
    emit storageUsageChanged();
    const auto context = m_storage;
    auto *watcher = new QFutureWatcher<storage::CleanupReport>(this);
    connect(watcher, &QFutureWatcher<storage::CleanupReport>::finished, this, [this, watcher, category] {
        const auto report = watcher->result();
        watcher->deleteLater();
        m_storageBusy = false;
        QString summary;
        if (report.released.isEmpty() && report.skipped.isEmpty() && report.failed.isEmpty())
            summary = QStringLiteral("没有需要清理的%1。").arg(category == QLatin1String("logs") ? QStringLiteral("历史日志") :
                category == QLatin1String("cache") ? QStringLiteral("闲置缓存") : QStringLiteral("无主数据"));
        else
            summary = QStringLiteral("已释放 %1 项（%2 MiB）").arg(report.released.size())
                .arg(QString::number(report.releasedBytes / (1024.0 * 1024.0), 'f', 1));
        if (!report.skipped.isEmpty()) summary += QStringLiteral("；在用跳过 %1 项").arg(report.skipped.size());
        if (!report.failed.isEmpty()) summary += QStringLiteral("；失败 %1 项").arg(report.failed.size());
        if (!report.error.isEmpty()) summary += QStringLiteral("\n%1").arg(report.error);
        feedback(summary);
        emit pluginNoticeRequested(QStringLiteral("存储清理"), summary);
        emit storageUsageChanged();
        // Re-scan asynchronously so the preview and orphan list reflect the result.
        requestStorageScan();
    });
    watcher->setFuture(QtConcurrent::run([context, category, ids] {
        storage::StorageMaintenance maintenance(context);
        if (category == QLatin1String("logs")) return maintenance.cleanupLogs();
        if (category == QLatin1String("cache")) return maintenance.cleanupCaches();
        return maintenance.cleanupOrphanData(ids);
    }));
}
QString AppController::localName(const QString &kind, const QString &id, const QString &fallback) const
{
    const auto value = m_settings.value("names/" + kind + '/' + id).toString().trimmed();
    return value.isEmpty() ? (fallback.isEmpty() ? id : fallback) : value;
}
QVariantList AppController::menus() const
{
    QVariantList result{QVariantMap{{"id", QString::fromLatin1(api::ZeroTierMenuId)}, {"title", QStringLiteral("ZeroTier")}, {"ready", true}, {"message", QString()}}};
    for (const auto &menu : m_registry.menus()) {
        QByteArray icon; QBuffer buffer(&icon); buffer.open(QIODevice::WriteOnly);
        if (!menu.icon.isNull()) menu.icon.pixmap(20,20).save(&buffer,"PNG");
        result.append(QVariantMap{{"id", menu.id}, {"title", menu.title}, {"iconSource", icon.isEmpty() ? QString() : "data:image/png;base64," + QString::fromLatin1(icon.toBase64())},
        {"ready", m_registry.preparationState(menu.pluginId) == plugins::PreparationState::Ready},
        {"loading", m_registry.preparationState(menu.pluginId) == plugins::PreparationState::Preparing},
        {"message", m_registry.preparationMessage(menu.pluginId)}, {"toolTip", menu.toolTip}});
    }
    for(const auto &value:m_loader.catalog()) {
        const auto row=value.toMap();
        if(!row["loading"].toBool() || row["loaded"].toBool()) continue;
        result.append(QVariantMap{{"id",row["path"].toString()+"/loading"},{"title",row["name"].toString()},
            {"ready",false},{"loading",true},{"message",QStringLiteral("插件加载中…")}});
    }
    return result;
}
QVariantList AppController::plugins() const { return m_loader.catalog(); }
void AppController::setWindow(QQuickWindow *window)
{
    m_window = window;
    m_windowId = 0;
    if (!window) return;
    window->setPersistentSceneGraph(false);
    window->setPersistentGraphics(false);
    // Cache the handle outside native event dispatch. Calling winId() from an
    // event filter while CreateWindowEx is in progress recursively creates it.
    m_windowId = window->winId();
#ifdef Q_OS_WIN
    // QAbstractNativeEventFilter only covers dispatched messages; Windows can
    // send caption commands synchronously, so subclass the native Quick window.
    if (QGuiApplication::platformName() == "windows")
        SetWindowSubclass(reinterpret_cast<HWND>(m_windowId), captionProcedure, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this));
#endif
    connect(window, &QWindow::visibleChanged, this, &AppController::setVisible);
    connect(window, &QWindow::windowStateChanged, this, [this] { setVisible(m_window && m_window->isVisible() && m_window->windowState() != Qt::WindowMinimized); });
    setVisible(window->isVisible() && window->windowState() != Qt::WindowMinimized);
#if defined(Q_OS_MACOS) && !defined(ZTO_TESTING)
    if (!zerotier::executable().isEmpty()) {
        setBusy(true);
        m_operations->runElevatedScript(QStringLiteral("授权读取 ZeroTier"), zerotier::wrapScript({}), [this](bool) { setBusy(false); refresh(); });
    }
#endif
}
void AppController::setVisible(bool visible) { m_visible = visible; updatePolling(); }
void AppController::updatePolling()
{
    const bool active = m_visible && !m_closing;
    if (m_serviceMonitor) m_serviceMonitor(active);
    if (active && !m_statusTimer.isActive()) { m_statusTimer.start(); updateService(); }
    else if (!active) m_statusTimer.stop();
    m_infoPoller->setActive(active);
    m_networkPoller->setActive(active && m_activeMenu == QLatin1String(api::ZeroTierMenuId));
    m_peerPoller->setActive(active && m_activeMenu == QLatin1String(api::ZeroTierMenuId));
    m_moonPoller->setActive(active && m_moonsActive);
    const bool membersActive = active && m_activeMenu == QLatin1String(api::ZeroTierMenuId);
    if (membersActive && !m_memberTimer.isActive()) { m_memberTimer.start(); refreshMembers(); }
    else if (!membersActive) {
        m_memberTimer.stop();
        const auto requests = std::move(m_memberRequests); m_memberRequests.clear();
        const auto probes = std::move(m_memberProbeContexts); m_memberProbeContexts.clear();
        for (auto context : requests) delete context.data();
        for (auto context : probes) delete context.data();
    }
}
void AppController::setMoonsActive(bool active) { m_moonsActive = active; updatePolling(); if (active) { refreshMoonFiles(); m_moonPoller->refresh(); } emit stateChanged(); }
bool AppController::navigate(const QString &menuId, QQuickItem *container)
{
    if (m_closing || !container) return false;
    QQuickItem *next = nullptr;
    if (menuId != QLatin1String(api::ZeroTierMenuId) && menuId != QLatin1String(api::SettingsMenuId)) {
        auto *engine = qmlEngine(container);
        const auto page = m_registry.page(menuId, engine, container);
        if (!page.result.ok()) { feedback(page.result.message); return false; }
        next = page.page;
    }
    const auto result = m_registry.activateMenu(menuId, [this, menuId, next, container] {
        if (m_page) m_page->setVisible(false);
        m_page = next;
        if (next) {
            next->setWidth(container->width()); next->setHeight(container->height()); next->setVisible(true);
            if (!next->property("hostResizeConnected").toBool()) {
                connect(container, &QQuickItem::widthChanged, next, [next, container] { next->setWidth(container->width()); });
                connect(container, &QQuickItem::heightChanged, next, [next, container] { next->setHeight(container->height()); });
                next->setProperty("hostResizeConnected", true);
            }
        }
        m_activeMenu = menuId;
    });
    if (!result.ok()) { feedback(result.message); return false; }
    updatePolling(); emit activeMenuChanged(); return true;
}
void AppController::refresh()
{
    updateService(); refreshMoonFiles(); m_infoPoller->refresh(); m_networkPoller->refresh(); m_peerPoller->refresh(); m_moonPoller->refresh(); refreshMembers();
}
void AppController::setBusy(bool value)
{
    m_busy = value; emit stateChanged();
    if (!value && m_closing && m_shutdownReady) emit exitFinished();
}
void AppController::feedback(const QString &message) { m_feedback = message; emit stateChanged(); }
void AppController::ask(const QString &title, const QString &message, const QString &acceptLabel, std::function<void()> action)
{
    if (m_closing || m_confirmation) return;
    m_confirmation = std::move(action); emit confirmationRequested(title, message, acceptLabel);
}
void AppController::confirm(bool accepted)
{
    auto action = std::move(m_confirmation); m_confirmation = {};
    if (accepted && !m_closing && action) action();
}
void AppController::serviceAction(const QString &action)
{
    if (m_busy || m_closing || !m_serviceStatus.known || (action != "start" && action != "stop" && action != "restart")) return;
    setBusy(true); feedback({});
    m_operations->runService(action, [this](bool ok) { setBusy(false); feedback(ok ? QString() : QStringLiteral("操作失败，请查看 logs 文件夹中的本次启动日志。")); refresh(); });
}
void AppController::packageAction(bool uninstall)
{
    if (m_busy || m_closing) return;
#ifdef Q_OS_MACOS
    const QString winget;
#else
    const QString winget = QStandardPaths::findExecutable("winget");
    if (winget.isEmpty()) { feedback(QStringLiteral("未找到 winget，请安装 Windows 应用安装程序后重试。")); return; }
#endif
    const auto title = uninstall ? QStringLiteral("卸载 ZeroTier") : QStringLiteral("安装 ZeroTier");
    const auto message = uninstall ? QStringLiteral("卸载 ZeroTier，并删除 %1 中的全部数据（包括节点身份和网络配置），确定继续？").arg(zerotier::dataDirectory())
#ifdef Q_OS_MACOS
        : QStringLiteral("从 ZeroTier 官方下载并安装 macOS 安装包，是否继续？");
#else
        : QStringLiteral("使用 winget 下载并安装 ZeroTier，并接受软件包与来源协议，是否继续？");
#endif
    ask(title, message, uninstall ? QStringLiteral("卸载并删除数据") : QStringLiteral("继续安装"), [this, title, winget, uninstall] {
        if (m_busy) return;
        setBusy(true);
        m_operations->runElevatedScript(title, zerotier::packageScript(uninstall, winget), [this](bool ok) {
            setBusy(false); feedback(ok ? QString() : QStringLiteral("操作失败，请查看本次启动日志。")); refresh();
        });
    });
}
void AppController::networkCommand(const QString &action, const QString &input, bool moonConfigured)
{
    if (m_busy || m_closing) return;
    const auto id = input.trimmed().toLower();
    if (!QRegularExpression("^[0-9a-f]{16}$").match(id).hasMatch()) { feedback(QStringLiteral("请输入完整的 16 位十六进制网络 ID。")); return; }
    setBusy(true); feedback({});
    zerotier::command(this, {action, id}, [this, action, moonConfigured](bool ok, const QString &output) {
        setBusy(false); util::log::write(output);
        feedback(ok ? QString() : (moonConfigured ? QStringLiteral("中转站已配置，但加入网络失败，可再次确认重试。\n") : QString())
            + (output.isEmpty() ? QStringLiteral("操作失败，请检查服务和管理员权限。") : output));
        if (ok) { if (action == "join") emit networkJoined(); m_networkPoller->refresh(); }
    });
}
void AppController::joinNetwork(const QString &id) { networkCommand("join", id); }
void AppController::leaveNetwork(const QString &id)
{
    if (m_busy || m_closing || m_networks.record(id).isEmpty()) return;
    ask(QStringLiteral("退出网络"), QStringLiteral("退出网络 %1？").arg(id), QStringLiteral("退出网络"), [this, id] { networkCommand("leave", id); });
}
void AppController::setExpanded(const QString &id, bool expanded)
{
    m_networks.update(id, {{"expanded", expanded}});
    if (!expanded) { delete m_memberRequests.take(id).data(); delete m_memberProbeContexts.take(id).data(); } else refreshMembers();
}
void AppController::refreshMembers()
{
    if (!m_visible || m_closing || m_activeMenu != QLatin1String(api::ZeroTierMenuId)) return;
    for (int i = 0; i < m_networks.count(); ++i) {
        const auto record = m_networks.get(i);
        const auto id = record["rowId"].toString();
        if (!record["expanded"].toBool() || m_memberRequests.value(id)) continue;
        const auto server = protocol::networkServer(QJsonObject::fromVariantMap(record["details"].toMap()));
        auto *context = new QObject(this); m_memberRequests.insert(id, context);
        zerotier::requestMembers(context, server, id, [this, context, id, server](const util::JsonResult &result) {
            m_memberRequests.remove(id); context->deleteLater();
            const auto current = m_networks.record(id);
            if (!current["expanded"].toBool() || protocol::networkServer(QJsonObject::fromVariantMap(current["details"].toMap())) != server) return;
            if (!result.ok()) { m_networks.update(id, {{"memberStatus", QStringLiteral("成员列表不可用：%1（每 5 秒重试）").arg(result.error)}}); return; }
            QVariantList members; QSet<QString> seen;
            for (const auto &value : result.document.array()) {
                const auto object = value.toObject(); const auto memberId = object["nodeId"].toString().toLower();
                if (!QRegularExpression("^[0-9a-f]{10}$").match(memberId).hasMatch() || seen.contains(memberId)) continue;
                seen.insert(memberId); const auto ips = addresses(object["ipAssignments"].toArray());
                QString latency=QStringLiteral("—");
                QString latencySource=QStringLiteral("尚无 ICMP ping 结果");
                for(const auto &old:current["members"].toList()) if(old.toMap()["id"].toString()==memberId) {
                    latency=old.toMap()["latency"].toString(); latencySource=old.toMap()["latencySource"].toString();
                }
                members.append(QVariantMap{{"id",memberId},{"name",localName("peers",memberId)},{"ip",ips.isEmpty()?QStringLiteral("—"):ips.join(", ")},
                    {"latency",latency},{"latencySource",latencySource}});
            }
            const auto ipKey = [](const QVariant &member) {
                const auto text = member.toMap()["ip"].toString().section(',', 0, 0).trimmed();
                const QHostAddress address(text);
                bool ipv4 = false;
                const auto number = address.toIPv4Address(&ipv4);
                return ipv4 ? quint64(number) : std::numeric_limits<quint64>::max();
            };
            std::stable_sort(members.begin(), members.end(), [&](const auto &a, const auto &b) {
                return ipKey(a) == ipKey(b) ? a.toMap()["id"].toString() < b.toMap()["id"].toString() : ipKey(a) < ipKey(b);
            });
            m_networks.update(id, {{"members", members}, {"memberStatus", members.isEmpty() ? QStringLiteral("暂无网络成员") : QString()}});
            auto *probes=m_memberProbeContexts.value(id).data();
            if(!probes) { probes=new QObject(this); m_memberProbeContexts[id]=probes; }
            for(const auto &value:members) {
                const auto target=value.toMap()["id"].toString();
                if (probes->findChild<QObject *>(target, Qt::FindDirectChildrenOnly)) continue;
                QHostAddress address;
                for (const auto &ip : value.toMap()["ip"].toString().split(',')) {
                    const QHostAddress candidate(ip.trimmed());
                    if (candidate.protocol() == QAbstractSocket::IPv4Protocol) { address = candidate; break; }
                }
                auto *job = new QObject(probes); job->setObjectName(target);
                const auto ips = value.toMap()["ip"].toString();
                zerotier::pingMember(job,address,[this,id,target,ips,job](zerotier::PingResult result) {
                    job->deleteLater();
                    const auto current=m_networks.record(id); if(!current["expanded"].toBool()) return;
                    auto rows=current["members"].toList();
                    for(auto &value:rows) { auto row=value.toMap(); if(row["id"].toString()!=target || row["ip"].toString()!=ips) continue;
                        // Only the member latency projection maps an unreachable probe to
                        // ASCII "-"; the detailed reason stays in the tooltip.
                        row["latency"]=result.state==zerotier::PingState::Unreachable ? QStringLiteral("-") : result.text;
                        row["latencyState"]=static_cast<int>(result.state);
                        row["latencySource"]=result.detail; value=row;
                    }
                    m_networks.update(id,{{"members",rows}});
                });
            }
        });
    }
}
void AppController::rename(const QString &kind, const QString &id, const QString &input)
{
    if (m_closing || id.isEmpty() || (kind != "networks" && kind != "peers" && kind != "moons")) return;
    const auto name = input.trimmed().left(128); const auto key = "names/" + kind + '/' + id;
    if (name.isEmpty() || name == id) m_settings.remove(key); else m_settings.setValue(key, name);
    auto *model = kind == "networks" ? &m_networks : kind == "peers" ? &m_peers : &m_moons;
    const auto fallback = kind == "networks" ? model->record(id)["details"].toMap()["name"].toString() : QString();
    model->update(id, {{"name", localName(kind, id, fallback)}});
    if (kind == "peers") {
        for (int n = 0; n < m_networks.count(); ++n) {
            const auto record = m_networks.get(n); auto members = record["members"].toList();
            for (auto &value : members) { auto member = value.toMap(); if (member["id"].toString() == id) { member["name"] = localName("peers", id); value = member; } }
            m_networks.update(record["rowId"].toString(), {{"members", members}});
        }
    }
}
void AppController::copyText(const QString &text) { QGuiApplication::clipboard()->setText(text); }
QVariantMap AppController::details(const QString &kind, const QString &id) const
{
    const auto record = kind == "networks" ? m_networks.record(id) : kind == "peers" ? m_peers.record(id) : m_moons.record(id);
    auto object = record["details"].toMap(); if (!record.isEmpty()) object["localName"] = record["name"]; return object;
}
QString AppController::detailsJson(const QString &kind, const QString &id) const
{
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(details(kind, id))).toJson(QJsonDocument::Indented));
}
QVariantList AppController::detailFields(const QString &kind, const QString &id) const
{
    QVariantList result; const auto object = QJsonObject::fromVariantMap(details(kind, id));
    std::function<void(const QString &, const QJsonValue &)> append = [&](const QString &key, const QJsonValue &value) {
        if (value.isObject()) { const auto object = value.toObject(); for (auto it = object.begin(); it != object.end(); ++it) append(key + '.' + it.key(), it.value()); }
        else if (value.isArray()) { const auto array = value.toArray(); for (int i = 0; i < array.size(); ++i) append(key + '[' + QString::number(i) + ']', array[i]); }
        else result.append(QVariantMap{{"key", key}, {"value", zerotier::jsonText(value)}});
    };
    for (auto it = object.begin(); it != object.end(); ++it) append(it.key(), it.value());
    return result;
}
bool AppController::hasMoon(const QString &id) const { return !zerotier::matchingMoonFile(id).isEmpty(); }
void AppController::refreshMoonFiles()
{
    if (m_closing) return;
    if (m_moonFilesPending) { m_moonRefreshQueued = true; return; }
    m_moonFilesPending = true;
    auto *watcher = new QFutureWatcher<QList<QVariantMap>>(this);
    connect(watcher, &QFutureWatcher<QList<QVariantMap>>::finished, this, [this, watcher] {
        auto rows = watcher->result(); watcher->deleteLater(); m_moonFilesPending = false;
        if (m_closing) return;
        for (auto &record : rows) record["name"] = localName("moons", record["rowId"].toString());
        m_moons.updateRows(rows);
        if (m_moonRefreshQueued) { m_moonRefreshQueued = false; refreshMoonFiles(); }
    });
    watcher->setFuture(QtConcurrent::run([document = m_moonMetadata, directory = zerotier::moonDirectory()] {
    QHash<QString, QJsonObject> metadata;
    for (const auto &value : document.array()) { const auto object = value.toObject(); metadata.insert(normalizeMoonId(object["id"].toString()), object); }
    QList<QVariantMap> rows;
    for (const auto &file : QDir(directory).entryInfoList(QDir::Files, QDir::Name)) {
        if (file.suffix().compare("moon", Qt::CaseInsensitive)) continue;
        const auto id = file.completeBaseName(); auto object = metadata.value(normalizeMoonId(id));
        object["id"] = normalizeMoonId(id); object["moonFile"] = file.absoluteFilePath();
        rows.append(row(id, id, {}, object));
        metadata.remove(normalizeMoonId(id));
    }
    // Orbit-loaded moons may have no local file (including the macOS session).
    auto loadedIds = metadata.keys(); std::sort(loadedIds.begin(), loadedIds.end());
    for (const auto &id : loadedIds) rows.append(row(id, id, {}, metadata.value(id)));
    return rows;
    }));
}
QVariantMap AppController::localConfig() const
{
    const auto parsedRelay = !m_localConfig.relayIp.isEmpty()
        ? zerotier::localconfig::displayRelay(m_localConfig.relayIp, m_localConfig.relayPort) : QString();
    return {{"path", m_localConfigPath},
            {"exists", m_localConfig.exists},
            {"loaded", m_localConfigLoaded},
            {"busy", m_localConfigBusy},
            {"valid", m_localConfig.valid()},
            {"allowTcpFallbackRelay", m_localConfig.allowTcpFallbackRelay},
            {"forceTcpRelay", m_localConfig.forceTcpRelay},
            {"relay", parsedRelay},
            {"relayText", zerotier::localconfig::relayText(m_localConfig)},
            {"error", m_localConfigError.isEmpty() ? m_localConfig.error : m_localConfigError},
            {"canRetry", !m_localConfigBusy && !m_localConfigError.isEmpty()}};
}

void AppController::loadLocalConfig(bool allowElevation)
{
    if (m_localConfigBusy || m_closing) return;
    m_localConfigPath = zerotier::localConfigPath();
    m_localConfigBusy = true;
    emit localConfigChanged();
    auto result = zerotier::localconfig::read(m_localConfigPath);
    bool permissionDenied = result.permissionDenied;
    QString error = result.error;
    if (result.ok && !result.state.exists) {
        const auto created = zerotier::localconfig::createDefault(m_localConfigPath);
        if (!created.ok) {
            permissionDenied = created.permissionDenied;
            error = created.error;
        } else {
            result = zerotier::localconfig::read(m_localConfigPath);
        }
    }
    if (result.ok && result.state.valid()) {
        applyLocalConfig(result.state, QString());
        return;
    }
    if (!result.ok) {
        if (error.isEmpty()) error = result.error;
        permissionDenied = permissionDenied || result.permissionDenied;
    }
    if (permissionDenied && allowElevation) {
        beginElevatedLocalConfigRead();
        return;
    }
    if (result.state.exists && !result.state.valid()) {
        applyLocalConfig(result.state, error.isEmpty() ? result.state.error : error);
        return;
    }
    recoverLocalConfig(error.isEmpty() ? QStringLiteral("无法读取 local.conf。") : error);
}

void AppController::beginElevatedLocalConfigRead()
{
    auto directory = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/zto-localconf-XXXXXX");
    if (!directory->isValid()) {
        recoverLocalConfig(QStringLiteral("无法创建读取 local.conf 的临时目录。"));
        return;
    }
    const auto snapshot = directory->filePath("local.conf");
    const auto script = zerotier::localconfig::ensureScript(m_localConfigPath, snapshot);
    if (script.isEmpty()) {
        recoverLocalConfig(QStringLiteral("当前平台不支持自动提权读取 ZeroTier 系统配置，请以管理员权限运行或调整目录权限。"));
        return;
    }
    m_localConfigBusy = true;
    emit localConfigChanged();
    m_operations->runElevatedScript(QStringLiteral("读取 ZeroTier 配置"), script, [this, directory, snapshot](bool ok) {
        if (m_closing) return;
        if (!ok) {
            recoverLocalConfig(QStringLiteral("无法读取或创建 local.conf（授权被取消或权限不足）。"));
            return;
        }
        const auto result = zerotier::localconfig::read(snapshot);
        if (!result.ok || !result.state.exists) {
            recoverLocalConfig(QStringLiteral("无法读取 local.conf 的系统配置快照。"));
            return;
        }
        applyLocalConfig(result.state, result.state.valid() ? QString() : result.error);
    });
}

void AppController::refreshLocalConfig()
{
    if (m_localConfigBusy || m_closing) return;
    const auto result = zerotier::localconfig::read(m_localConfigPath);
    if (result.ok && result.state.exists && result.state.valid()) {
        applyLocalConfig(result.state, QString());
        return;
    }
    if (result.ok && !result.state.exists) {
        // Keep the startup rule: a missing file is created with the documented
        // initial content rather than merely projecting defaults.
        loadLocalConfig(false);
        return;
    }
    // Reading the protected system file may require the authorization that the
    // last load already obtained. Keep that snapshot instead of prompting on
    // every settings-page navigation; the explicit re-read performs a new read.
    if (!result.ok && result.permissionDenied && m_localConfigLoaded && m_localConfig.valid()) return;
    loadLocalConfig(false);
}

void AppController::retryLocalConfig()
{
    if (m_localConfigBusy) return;
    loadLocalConfig(true);
}

void AppController::applyLocalConfig(const zerotier::localconfig::State &state, const QString &error)
{
    m_localConfig = state;
    m_localConfigLoaded = true;
    m_localConfigBusy = false;
    m_localConfigError = error.isEmpty() ? state.error : error;
    emit localConfigChanged();
    emit stateChanged();
}

void AppController::recoverLocalConfig(const QString &error)
{
    const auto result = zerotier::localconfig::read(m_localConfigPath);
    if (result.ok) {
        applyLocalConfig(result.state, error);
        return;
    }
    if (result.state.exists && !result.state.valid()) {
        applyLocalConfig(result.state, error + QLatin1Char('\n') + result.state.error);
        return;
    }
    m_localConfigLoaded = false;
    m_localConfigBusy = false;
    m_localConfigError = error;
    emit localConfigChanged();
    emit stateChanged();
}

bool AppController::currentLocalConfigForUpdate(zerotier::localconfig::State &state, QString &expectedHash, QString &error)
{
    const auto result = zerotier::localconfig::read(m_localConfigPath);
    if (result.ok) {
        if (result.state.exists && !result.state.valid()) {
            error = result.state.error;
            return false;
        }
        state = result.state;
        expectedHash = state.exists ? sha256Hex(state.raw) : QString();
        error.clear();
        return true;
    }
    // The app may have read the protected system file through its existing
    // elevation path. Reuse that validated snapshot, and let the elevated
    // installer verify the current file hash before replacing it.
    if (result.permissionDenied && m_localConfigLoaded && m_localConfig.valid()) {
        state = m_localConfig;
        expectedHash = state.exists ? sha256Hex(state.raw) : QString();
        error.clear();
        return true;
    }
    error = result.error;
    return false;
}

void AppController::writeLocalConfig(const QByteArray &content, const QString &expectedHash,
                                     std::function<void(bool, const QString &)> finished)
{
    const auto direct = zerotier::localconfig::writeAtomic(m_localConfigPath, content);
    if (direct.ok) {
        applyLocalConfig(zerotier::localconfig::parse(content, true), QString());
        if (finished) finished(true, QString());
        return;
    }
    if (!direct.permissionDenied) {
        recoverLocalConfig(direct.error);
        if (finished) finished(false, direct.error);
        return;
    }
    auto directory = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/zto-localconf-save-XXXXXX");
    if (!directory->isValid()) {
        const auto message = QStringLiteral("无法创建保存 local.conf 的临时目录。");
        recoverLocalConfig(message);
        if (finished) finished(false, message);
        return;
    }
    const auto proposed = directory->filePath("local.conf");
    QFile file(proposed);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.flush()) {
        const auto message = QStringLiteral("无法准备 local.conf 的保存内容。");
        recoverLocalConfig(message);
        if (finished) finished(false, message);
        return;
    }
    file.close();
    const auto script = zerotier::localconfig::installScript({m_localConfigPath, proposed, expectedHash});
    if (script.isEmpty()) {
        const auto message = QStringLiteral("当前平台不支持自动提权写入 ZeroTier 系统配置，请以管理员权限运行或调整目录权限。");
        recoverLocalConfig(message);
        if (finished) finished(false, message);
        return;
    }
    m_operations->runElevatedScript(QStringLiteral("保存 ZeroTier 配置"), script,
        [this, directory, content, finished](bool ok) {
            if (!ok) {
                const auto message = QStringLiteral("无法保存 local.conf（授权被取消、权限不足，或文件已被其他程序修改）。");
                recoverLocalConfig(message);
                if (finished) finished(false, message);
                return;
            }
            applyLocalConfig(zerotier::localconfig::parse(content, true), QString());
            if (finished) finished(true, QString());
        });
}

void AppController::setTcpFallbackSwitches(bool allow, bool force)
{
    if (m_localConfigBusy || m_closing) return;
    if (!m_localConfigLoaded || !m_localConfig.valid()) {
        feedback(QStringLiteral("本地 ZeroTier 配置当前不可用，未保存 TCP 回退设置。"));
        return;
    }
    if (!allow) force = false;
    if (allow == m_localConfig.allowTcpFallbackRelay && force == m_localConfig.forceTcpRelay) return;
    m_localConfigBusy = true;
    emit localConfigChanged();
    zerotier::localconfig::State base;
    QString expectedHash;
    QString error;
    if (!currentLocalConfigForUpdate(base, expectedHash, error)) {
        recoverLocalConfig(error);
        return;
    }
    QByteArray content;
    if (!zerotier::localconfig::mergeSwitches(base, allow, force, content, error)) {
        recoverLocalConfig(error);
        return;
    }
    writeLocalConfig(content, expectedHash, [](bool, const QString &) {});
}

void AppController::refreshTcpFallbackServers()
{
    if (m_tcpFallbackServersBusy || m_closing) return;
    m_tcpFallbackServersBusy = true;
    m_tcpFallbackServersMessage = QStringLiteral("正在读取网络与 Moon 元数据…");
    emit tcpFallbackServersChanged();
    zerotier::command(this, {"-j", "listnetworks"}, [this](bool ok, const QString &output) {
        if (m_closing) return;
        if (!ok) {
            m_tcpFallbackServersBusy = false;
            m_tcpFallbackServersMessage = QStringLiteral("无法读取网络列表：%1").arg(output.isEmpty() ? QStringLiteral("ZeroTier 未响应") : output);
            emit tcpFallbackServersChanged();
            return;
        }
        QJsonParseError parseError;
        const auto networks = QJsonDocument::fromJson(output.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !networks.isArray()) {
            m_tcpFallbackServersBusy = false;
            m_tcpFallbackServersMessage = QStringLiteral("网络列表响应无效。");
            emit tcpFallbackServersChanged();
            return;
        }
        zerotier::command(this, {"-j", "listmoons"}, [this, networks](bool moonOk, const QString &moonOutput) {
            if (m_closing) return;
            QJsonDocument moons;
            QString moonError;
            if (moonOk) {
                QJsonParseError error;
                moons = QJsonDocument::fromJson(moonOutput.toUtf8(), &error);
                if (error.error != QJsonParseError::NoError || !moons.isArray()) {
                    moonError = QStringLiteral("Moon 元数据响应无效。");
                    moons = QJsonDocument(QJsonArray{});
                }
            } else {
                moonError = QStringLiteral("无法读取 Moon 元数据：%1")
                    .arg(moonOutput.isEmpty() ? QStringLiteral("ZeroTier 未响应") : moonOutput);
            }
            const auto list = zerotier::localconfig::candidates(networks.array(), moons,
                [](const QString &id) { return !zerotier::matchingMoonFile(id).isEmpty(); }, m_localConfig);
            m_tcpFallbackServers.clear();
            for (const auto &candidate : list) {
                m_tcpFallbackServers.append(QVariantMap{
                    {"networkId", candidate.networkId},
                    {"name", localName("networks", candidate.networkId, candidate.name)},
                    {"ip", candidate.ip}, {"relay", candidate.relay},
                    {"reason", candidate.reason}, {"note", candidate.note},
                    {"enabled", candidate.enabled}, {"current", candidate.current}});
            }
            m_tcpFallbackServersBusy = false;
            if (!moonError.isEmpty()) m_tcpFallbackServersMessage = moonError;
            else if (list.isEmpty()) m_tcpFallbackServersMessage =
                QStringLiteral("没有符合导出条件的网络（需要本地存在对应的 Moon 文件）。");
            else m_tcpFallbackServersMessage.clear();
            emit tcpFallbackServersChanged();
        });
    });
}

void AppController::selectTcpFallbackServer(const QString &networkId)
{
    if (m_localConfigBusy || m_closing) return;
    if (!m_localConfigLoaded || !m_localConfig.valid()) {
        feedback(QStringLiteral("本地 ZeroTier 配置当前不可用，未切换 TCP 回退中转服务器。"));
        return;
    }
    QVariantMap selected;
    for (const auto &value : m_tcpFallbackServers) {
        const auto row = value.toMap();
        if (row["networkId"].toString().compare(networkId, Qt::CaseInsensitive) == 0) { selected = row; break; }
    }
    const auto ip = selected["ip"].toString();
    if (ip.isEmpty() || !selected["enabled"].toBool()) return;
    if (m_localConfig.relayPort == zerotier::localconfig::RelayPort && m_localConfig.relayIp == ip) {
        feedback(QStringLiteral("当前已使用该 TCP 回退中转服务器。"));
        return;
    }
    const auto address = zerotier::localconfig::displayRelay(ip, zerotier::localconfig::RelayPort);
    ask(QStringLiteral("切换 TCP 回退中转服务器"),
        QStringLiteral("将本机 ZeroTier 的 TCP 回退中转服务器切换为 %1？\n\n"
                       "这是本机节点级设置，影响本机加入的所有网络；保存后不会自动重启 ZeroTier。").arg(address),
        QStringLiteral("切换"), [this, networkId, ip] { applyTcpFallbackRelay(networkId, ip); });
}

void AppController::applyTcpFallbackRelay(const QString &networkId, const QString &ip)
{
    if (m_localConfigBusy || m_closing || ip.isEmpty()) return;
    m_localConfigBusy = true;
    emit localConfigChanged();
    zerotier::localconfig::State base;
    QString expectedHash;
    QString error;
    if (!currentLocalConfigForUpdate(base, expectedHash, error)) {
        recoverLocalConfig(error);
        return;
    }
    QByteArray content;
    if (!zerotier::localconfig::mergeRelay(base, ip, zerotier::localconfig::RelayPort, content, error)) {
        recoverLocalConfig(error);
        return;
    }
    writeLocalConfig(content, expectedHash, [this, networkId, ip](bool ok, const QString &) {
        if (!ok) return;
        for (auto &value : m_tcpFallbackServers) {
            auto row = value.toMap();
            row["current"] = row["networkId"].toString().compare(networkId, Qt::CaseInsensitive) == 0
                && row["ip"].toString() == ip;
            value = row;
        }
        emit tcpFallbackServersChanged();
        emit pluginNoticeRequested(QStringLiteral("TCP回退中转服务器"),
            QStringLiteral("TCP回退中转服务器已更新，请手动重启 ZeroTier，以及时刷新配置并切换中继连接。"));
    });
}

void AppController::revealLocalConfig()
{
    if (m_closing) return;
    const QFileInfo info(m_localConfigPath);
    if (!info.isFile()) {
        feedback(QStringLiteral("本地配置文件尚未创建：%1。请先重试读取或安装 ZeroTier。")
                     .arg(QDir::toNativeSeparators(m_localConfigPath)));
        return;
    }
    QString error;
    if (!m_operations->revealFile(info.absoluteFilePath(), error))
        feedback(error.isEmpty() ? QStringLiteral("无法在文件管理器中定位本地配置文件。") : error);
}
void AppController::exportConfiguration(const QString &id, const QUrl &destination)
{
    if (!destination.isLocalFile() || m_closing) return;
    const auto path = zerotier::matchingMoonFile(id); QFile source(path);
    if (path.isEmpty() || !source.open(QIODevice::ReadOnly)) { feedback(QStringLiteral("无法读取 Moon 文件。")); return; }
    const auto bytes = source.read(4 * 1024 * 1024 + 1);
    if (source.error() != QFileDevice::NoError || bytes.isEmpty()) { feedback(QStringLiteral("无法读取 Moon 文件。")); return; }
    // Re-read the actual system file at export time. If the protected file
    // cannot be read directly, the last validated snapshot is used rather than
    // silently exporting a different local.conf.
    auto local = m_localConfig;
    const auto fresh = zerotier::localconfig::read(m_localConfigPath);
    if (fresh.ok) local = fresh.state;
    else if (!fresh.permissionDenied) local = zerotier::localconfig::State{};
    const auto fileName = QFileInfo(path).fileName();
    zerotier::command(this, {"-j", "listmoons"}, [this, id, destination, fileName, bytes, local](bool ok, const QString &moonOutput) {
        if (m_closing) return;
        QString relay;
        if (ok) {
            QJsonParseError error;
            const auto moons = QJsonDocument::fromJson(moonOutput.toUtf8(), &error);
            if (error.error == QJsonParseError::NoError && moons.isArray())
                relay = zerotier::localconfig::exportRelay(local, id, moons);
        }
        const auto bundle = zerotier::bundle::encode({id, fileName, bytes, relay});
        auto target = destination.toLocalFile();
        if (!target.endsWith(".z2bundle", Qt::CaseInsensitive)) target += ".z2bundle";
        QSaveFile output(target);
        if (bundle.isEmpty() || !output.open(QIODevice::WriteOnly)
            || output.write(bundle) != bundle.size() || !output.commit())
            feedback(QStringLiteral("无法导出中转站配置包。"));
        else feedback(QStringLiteral("中转站配置包已导出。"));
    });
}
void AppController::importConfiguration(const QUrl &source)
{
    if (m_busy || m_closing || !source.isLocalFile()) return;
    const auto path = source.toLocalFile();
    if (path.endsWith(".moon", Qt::CaseInsensitive)) { importMoonFiles({path}); return; }
    QFile input(path); zerotier::bundle::Configuration configuration; QString error;
    if (!input.open(QIODevice::ReadOnly)) { feedback(QStringLiteral("无法读取配置包。")); return; }
    const auto bytes = input.read(zerotier::bundle::MaxFileSize + 1);
    if (input.error() != QFileDevice::NoError || !zerotier::bundle::decode(bytes, configuration, error)) { feedback(error.isEmpty() ? QStringLiteral("配置包读取失败。") : error); return; }
    auto temporary = std::make_shared<QTemporaryDir>(); const auto moonPath = temporary->filePath(configuration.moonFileName); QFile moon(moonPath);
    if (!temporary->isValid() || !moon.open(QIODevice::WriteOnly) || moon.write(configuration.moonContents) != configuration.moonContents.size() || !moon.flush()) { feedback(QStringLiteral("无法创建临时 Moon 文件。")); return; }
    moon.close(); emit configurationImported(configuration.networkId);
    importMoonFiles({moonPath}, [this, temporary, configuration](bool ok) { if (ok) networkCommand("join", configuration.networkId, true); },
                    configuration.tcpFallbackRelay);
}
void AppController::importMoons(const QList<QUrl> &sources)
{
    QStringList paths; for (const auto &url : sources) { if (!url.isLocalFile()) return; paths.append(url.toLocalFile()); }
    importMoonFiles(paths);
}
void AppController::importMoonFiles(const QStringList &sources, std::function<void(bool)> completed,
                                     const QString &tcpFallbackRelay)
{
    if (m_busy || m_closing) { if (completed) completed(false); return; }
    QList<QPair<QString, QString>> copies; QSet<QString> names; QStringList replacements;
    for (const auto &source : sources) {
        const QFileInfo file(source); if (!file.isFile() || file.suffix().compare("moon", Qt::CaseInsensitive)) { feedback(QStringLiteral("请选择有效的 .moon 文件。")); if (completed) completed(false); return; }
        const auto target = QDir(zerotier::moonWriteDirectory()).filePath(file.fileName());
        if (file.absoluteFilePath().compare(target, Qt::CaseInsensitive) == 0) continue;
        const auto name = file.fileName().toLower();
        if (names.contains(name)) { feedback(QStringLiteral("不能同时导入同名 Moon 文件。")); if (completed) completed(false); return; }
        names.insert(name);
        if (QFileInfo::exists(QDir(zerotier::moonDirectory()).filePath(file.fileName()))) replacements.append(file.fileName());
        copies.append({file.absoluteFilePath(), target});
    }
    if (copies.isEmpty()) { if (completed) completed(false); return; }
    const bool updateRelay = !tcpFallbackRelay.isEmpty();
    auto execute = [this, copies, completed, updateRelay, tcpFallbackRelay] {
        if (m_busy || m_closing) { if (completed) completed(false); return; }
        setBusy(true);
        auto configDirectory = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/zto-import-localconf-XXXXXX");
        zerotier::localconfig::Install install;
        QByteArray content;
        if (updateRelay) {
            QString error;
            if (!configDirectory->isValid()
                || !prepareImportRelay(tcpFallbackRelay, configDirectory->path(), install, content, error)) {
                setBusy(false);
                const auto message = error.isEmpty() ? QStringLiteral("无法准备 local.conf 的导入内容。") : error;
                feedback(message);
                emit pluginNoticeRequested(QStringLiteral("无法导入配置"), message);
                if (completed) completed(false);
                return;
            }
        }
        const auto script = zerotier::moonImportScript(copies, updateRelay ? &install : nullptr);
        if (script.isEmpty()) {
            setBusy(false);
            const auto message = QStringLiteral("当前平台不支持在导入配置时更新 local.conf，未执行导入。");
            feedback(message);
            emit pluginNoticeRequested(QStringLiteral("无法导入配置"), message);
            if (completed) completed(false);
            return;
        }
        m_operations->runElevatedScript(QStringLiteral("添加 Moon 并重启服务"), script,
            [this, completed, updateRelay, content, configDirectory](bool ok) {
                setBusy(false);
                if (updateRelay) {
                    if (ok) applyLocalConfig(zerotier::localconfig::parse(content, true), QString());
                    else {
                        const auto actual = zerotier::localconfig::read(m_localConfigPath);
                        if (actual.ok) applyLocalConfig(actual.state, QString());
                    }
                }
                refresh();
                if (ok) feedback({});
                else feedback(QStringLiteral("导入未完成；local.conf、Moon 文件或服务可能仅部分更新，请查看本次启动日志。"));
                if (completed) completed(ok);
            });
    };
    if (replacements.isEmpty()) execute();
    else ask(QStringLiteral("替换 Moon 文件"), replacements.join(QStringLiteral("、")) + QStringLiteral(" 已存在，确定替换并重启服务？"), QStringLiteral("替换并重启"), execute);
}
bool AppController::prepareImportRelay(const QString &display, const QString &directoryPath,
                                       zerotier::localconfig::Install &install, QByteArray &content, QString &error)
{
    QString ip;
    int port = 0;
    if (!zerotier::localconfig::parseDisplayRelay(display, ip, port) || port != zerotier::localconfig::RelayPort) {
        error = QStringLiteral("配置包的 tcpFallbackRelay 必须是有效的 IPv4:9443。");
        return false;
    }
    zerotier::localconfig::State base;
    QString expectedHash;
    if (!currentLocalConfigForUpdate(base, expectedHash, error)) return false;
    if (!base.valid()) { error = base.error; return false; }
    if (!zerotier::localconfig::mergeRelay(base, ip, port, content, error)) return false;
    const auto proposed = QDir(directoryPath).filePath("local.conf");
    QFile file(proposed);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.flush()) {
        error = QStringLiteral("无法准备 local.conf 的导入内容。");
        return false;
    }
    file.close();
    install = {m_localConfigPath, proposed, expectedHash};
    return true;
}

void AppController::deleteMoon(const QString &id)
{
    if (m_busy || m_closing) return;
    const auto record = m_moons.record(id); const auto path = record["details"].toMap()["moonFile"].toString(); if (path.isEmpty()) return;
    ask(QStringLiteral("删除 Moon"), QStringLiteral("确定删除“%1”？删除后重启服务。此操作无法撤销。").arg(record["name"].toString()), QStringLiteral("删除并重启"), [this, id, path] {
        if (m_busy) return;
        setBusy(true); m_operations->runElevatedScript(QStringLiteral("删除 Moon 并重启服务"), zerotier::moonDeleteScript(path), [this, id, path](bool ok) {
            if (!QFileInfo::exists(path)) m_settings.remove("names/moons/" + id);
            setBusy(false); refresh(); feedback(ok ? QString() : QStringLiteral("文件删除或服务启动未完成，请查看本次启动日志。"));
        });
    });
}
void AppController::importPlugin(const QUrl &source)
{
    if (m_closing || !source.isLocalFile()) return;
    const auto package=package::inspect(source.toLocalFile());
    const auto id=package.info["id"].toString();
    for(const auto &entry:m_loader.catalog()) if(!id.isEmpty() && entry.toMap()["id"].toString().compare(id,Qt::CaseInsensitive)==0) {
        emit pluginNoticeRequested(QStringLiteral("无法加载插件"),QStringLiteral("同 ID 插件已存在：%1。请先卸载并删除旧插件。ID 不区分大小写。").arg(id)); return;
    }
    const auto error = plugins::importPlugin(source.toLocalFile(), m_pluginDirectory);
    if(!error.isEmpty()) { feedback(error); emit pluginNoticeRequested(QStringLiteral("无法导入插件"),error); return; }
    const auto path=QDir(m_pluginDirectory).filePath(QFileInfo(source.toLocalFile()).fileName());
    const auto result=m_loader.loadFile(path,m_registry); feedback(result.ok()?QString():result.message);
    if(!result.ok()) emit pluginNoticeRequested(QStringLiteral("无法加载插件"),result.message);
}
void AppController::loadPlugin(const QString &id) {
    if(m_closing) return;
    for(const auto &value:m_loader.catalog()) if(value.toMap()["path"].toString()==id || value.toMap()["id"].toString().compare(id,Qt::CaseInsensitive)==0) {
        const auto result=m_loader.loadFile(value.toMap()["path"].toString(),m_registry); feedback(result.ok()?QString():result.message);
        if(!result.ok()) emit pluginNoticeRequested(QStringLiteral("无法加载插件"),result.message);
        return;
    }
}
void AppController::deletePlugin(const QString &path) {
    if(m_closing) return;
    for(const auto &entry:m_loader.catalog()) {
        const auto row=entry.toMap(); if(row["path"].toString()!=path) continue;
        if(row["loaded"].toBool() || row["loading"].toBool() || row["stopping"].toBool()) return;
        const auto plan=m_loader.deletionPlan(path);
        const bool dataRemovable=plan.value("dataRemovable").toBool();
        const auto message=dataRemovable
            ? QStringLiteral("确定删除“%1”？\n将从程序中移除此插件，并永久删除 plugins/ 下的插件包及 plugins-data/ 下对应的插件数据。此操作无法撤销。").arg(row["name"].toString())
            : QStringLiteral("确定清理“%1”？\n%2\n\n将只删除 plugins/ 下的插件包文件；插件数据、所有权记录和启用状态会保留，便于修复或重新安装。")
                  .arg(row["name"].toString(),plan.value("reason").toString());
        const auto removal=dataRemovable?plugins::PluginLoader::Removal::Full
                                         :plugins::PluginLoader::Removal::PackageOnly;
        ask(QStringLiteral("删除插件"),message,QStringLiteral("删除"),[this,path,removal] {
            const auto result=m_loader.removeFile(path,m_pluginDirectory,{},removal);
            feedback(result.message);
            refreshStorageUsage();
            if(!result.ok()) emit pluginNoticeRequested(QStringLiteral("无法删除插件"),result.message);
        }); return;
    }
}
void AppController::unloadPlugin(const QString &id) {
    if(m_closing) return;
    const auto result=m_loader.unload(id,m_registry);
    if(result.ok() && m_activeMenu.startsWith(id+"/",Qt::CaseInsensitive) && m_page)
        navigate(api::ZeroTierMenuId,m_page->parentItem());
    feedback(result.ok()?QString():result.message);
    if(!result.ok()) emit pluginNoticeRequested(QStringLiteral("无法卸载插件"),result.message);
}
void AppController::requestQuit()
{
    if (m_closing) { m_loader.requestShutdown(); return; }
    m_closing = true; m_confirmation = {}; updatePolling();
    for (auto context : m_memberRequests) delete context.data(); m_memberRequests.clear();
    for(auto context:m_memberProbeContexts) delete context.data(); m_memberProbeContexts.clear();
    m_shutdownMessage = QStringLiteral("正在退出，正在等待操作与插件结束…"); emit stateChanged();
    m_loader.requestShutdown();
}
void AppController::hideToTray() { if (!m_closing && m_window) emit trayRequested(); }
void AppController::restoreWindow() { if (!m_closing && m_window) platform::SingleInstance::activateWindow(*m_window); }
}
