#include "AppController.h"
#include "ClientNetworkService.h"
#include "NetworkTransport.h"
#include "PluginPackage.h"
#include "AppLog.h"
#include "NetworkBundle.h"
#include "NetworkMembers.h"
#include "PluginImport.h"
#include "SingleInstance.h"
#include <ztprotocol/NetworkAddress.h>
#include <QClipboard>
#include <QGuiApplication>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QSettings>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QJsonArray>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>
#include <QBuffer>
#include <algorithm>
#include <limits>
#ifdef Q_OS_WIN
#include <windows.h>
#include <commctrl.h>
#endif

namespace vusiif::ztgui::ui {
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
QString localName(const QString &kind, const QString &id, const QString &fallback = {})
{
    const auto value = QSettings().value("names/" + kind + '/' + id).toString().trimmed();
    return value.isEmpty() ? (fallback.isEmpty() ? id : fallback) : value;
}
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
}
AppController::AppController(QObject *parent, const QString &pluginDirectory, SystemOperations *operations)
    : QObject(parent), m_operations(operations ? operations : new SystemOperations(this)),
      m_infoPoller(new zerotier::JsonPoller(this, "info", true)),
      m_networkPoller(new zerotier::JsonPoller(this, "listnetworks")),
      m_peerPoller(new zerotier::JsonPoller(this, "listpeers")),
      m_moonPoller(new zerotier::JsonPoller(this, "listmoons")),
      m_pluginDirectory(pluginDirectory.isEmpty() ? QCoreApplication::applicationDirPath() + "/plugins" : pluginDirectory)
{
    m_serviceStatus.known = false;
    connect(&m_loader,&plugins::PluginLoader::stateChanged,this,&AppController::menusChanged);
    network::ClientNetworkService::instance();
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
        m_peerLatencies.clear();
        QList<QVariantMap> rows;
        for (const auto &value : doc.array()) {
            const auto object = value.toObject();
            const auto role = object["role"].toString();
            const int latency = object["latency"].toInt(-1);
            m_peerLatencies[object["address"].toString().toLower()]=latency;
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
    m_statusTimer.start();
    m_memberTimer.setInterval(2000);
    connect(&m_memberTimer, &QTimer::timeout, this, &AppController::refreshMembers);
    m_memberTimer.start();
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
#ifndef ZEROTIER_TESTING
    zerotier::startServiceMonitor(this);
    for (const auto &message : m_loader.scan(m_pluginDirectory, m_registry)) util::log::write(message);
#else
    if (!pluginDirectory.isEmpty()) m_loader.scan(m_pluginDirectory, m_registry);
#endif
    updateService(); refreshMoonFiles();
}
AppController::~AppController()
{
#ifdef Q_OS_WIN
    if (m_windowId && IsWindow(reinterpret_cast<HWND>(m_windowId)))
        RemoveWindowSubclass(reinterpret_cast<HWND>(m_windowId), captionProcedure, reinterpret_cast<UINT_PTR>(this));
#endif
}
QVariantMap AppController::service() const
{
    QString warning;
    if (!m_serviceStatus.known && !m_serviceQueryPending) warning = QStringLiteral("无法读取服务状态，请检查管理员权限。");
    else if (m_apiAvailable && m_info["tcpFallbackActive"].toBool()) warning = QStringLiteral("服务使用 TCP 中转，请检查网络连接。");
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
QString AppController::theme() const { return QSettings().value("appearance/theme", "light").toString() == "dark" ? "dark" : "light"; }
bool AppController::dark() const { return theme() == "dark"; }
void AppController::setTheme(const QString &value)
{
    if (value != "light" && value != "dark") return;
    QSettings().setValue("appearance/theme", value); emit themeChanged();
}
bool AppController::disableGpuAcceleration() const
{
    return QSettings().value("rendering/disableGpuAcceleration", false).toBool();
}
void AppController::setDisableGpuAcceleration(bool disabled)
{
    if (disableGpuAcceleration() == disabled) return;
    QSettings settings;
    settings.setValue("rendering/disableGpuAcceleration", disabled);
    settings.sync();
    emit disableGpuAccelerationChanged();
}
bool AppController::gpuAccelerationForcedOff() const
{
    return QCoreApplication::arguments().contains("--disable-gpu-acceleration");
}
QString AppController::version() const { return QCoreApplication::applicationVersion(); }
QString AppController::apiVersion() const { return QString::fromLatin1(api::ApiVersion); }
QString AppController::qtVersion() const { return QString::fromLatin1(qVersion()); }
QVariantList AppController::menus() const
{
    QVariantList result{QVariantMap{{"id", QString::fromLatin1(api::ZeroTierMenuId)}, {"title", QStringLiteral("ZeroTier")}, {"ready", true}, {"message", QString()}}};
    for (const auto &menu : m_registry.menus()) {
        QByteArray icon; QBuffer buffer(&icon); buffer.open(QIODevice::WriteOnly);
        if (!menu.icon.isNull()) menu.icon.pixmap(20,20).save(&buffer,"PNG");
        result.append(QVariantMap{{"id", menu.id}, {"title", menu.title}, {"iconSource", icon.isEmpty() ? QString() : "data:image/png;base64," + QString::fromLatin1(icon.toBase64())},
        {"ready", m_registry.preparationState(menu.pluginId) == plugins::PreparationState::Ready},
        {"message", m_registry.preparationMessage(menu.pluginId)}, {"toolTip", menu.toolTip}});
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
    setVisible(window->isVisible());
#if defined(Q_OS_MACOS) && !defined(ZEROTIER_TESTING)
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
    m_infoPoller->setActive(active);
    m_networkPoller->setActive(active && m_activeMenu == QLatin1String(api::ZeroTierMenuId));
    m_peerPoller->setActive(active && m_activeMenu == QLatin1String(api::ZeroTierMenuId));
    m_moonPoller->setActive(active && m_moonsActive);
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
            if (!result.ok()) { m_networks.update(id, {{"memberStatus", QStringLiteral("成员列表不可用：%1（2 秒后重试）").arg(result.error)}}); return; }
            QVariantList members; QSet<QString> seen;
            for (const auto &value : result.document.array()) {
                const auto object = value.toObject(); const auto memberId = object["nodeId"].toString().toLower();
                if (!QRegularExpression("^[0-9a-f]{10}$").match(memberId).hasMatch() || seen.contains(memberId)) continue;
                seen.insert(memberId); const auto ips = addresses(object["ipAssignments"].toArray());
                const int peer=m_peerLatencies.value(memberId,-1);
                const bool self=memberId==m_info["address"].toString();
                QString latency=self?"0 ms":peer>=0?QString::number(peer)+" ms":QStringLiteral("—");
                QString latencySource=self?QStringLiteral("本机"):peer>=0?QStringLiteral("ZeroTier peers 延迟"):QStringLiteral("尚无有效延迟");
                for(const auto &old:current["members"].toList()) if(old.toMap()["id"].toString()==memberId && old.toMap()["latencySource"].toString()==QStringLiteral("虚拟 IP 应用 RTT")) {
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
                network::ClientNetworkService::instance().transport().probeMember(id,target,probes,[this,id,target](double rtt) {
                    const auto current=m_networks.record(id); if(!current["expanded"].toBool()) return;
                    auto rows=current["members"].toList();
                    for(auto &value:rows) { auto row=value.toMap(); if(row["id"].toString()!=target) continue;
                        const int peer=m_peerLatencies.value(target,-1);
                        row["latency"]=rtt>=0?QString::number(rtt,'f',rtt==0?0:1)+" ms":peer>=0?QString::number(peer)+" ms":QStringLiteral("不可达");
                        row["latencySource"]=rtt>=0?QStringLiteral("虚拟 IP 应用 RTT"):peer>=0?QStringLiteral("ZeroTier peers 延迟"):QStringLiteral("应用探测超时或目标未提供接收服务"); value=row;
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
    if (name.isEmpty() || name == id) QSettings().remove(key); else QSettings().setValue(key, name);
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
void AppController::exportConfiguration(const QString &id, const QUrl &destination)
{
    if (!destination.isLocalFile() || m_closing) return;
    const auto path = zerotier::matchingMoonFile(id); QFile source(path);
    if (path.isEmpty() || !source.open(QIODevice::ReadOnly)) { feedback(QStringLiteral("无法读取 Moon 文件。")); return; }
    const auto bytes = source.read(4 * 1024 * 1024 + 1);
    const auto bundle = zerotier::bundle::encode({id, QFileInfo(path).fileName(), bytes});
    auto target = destination.toLocalFile(); if (!target.endsWith(".ztbundle", Qt::CaseInsensitive)) target += ".ztbundle";
    QSaveFile output(target);
    if (source.error() != QFileDevice::NoError || bundle.isEmpty() || !output.open(QIODevice::WriteOnly) || output.write(bundle) != bundle.size() || !output.commit()) feedback(QStringLiteral("无法导出中转站配置包。"));
    else feedback(QStringLiteral("中转站配置包已导出。"));
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
    importMoonFiles({moonPath}, [this, temporary, configuration](bool ok) { if (ok) networkCommand("join", configuration.networkId, true); });
}
void AppController::importMoons(const QList<QUrl> &sources)
{
    QStringList paths; for (const auto &url : sources) { if (!url.isLocalFile()) return; paths.append(url.toLocalFile()); }
    importMoonFiles(paths);
}
void AppController::importMoonFiles(const QStringList &sources, std::function<void(bool)> completed)
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
    auto execute = [this, copies, completed] {
        if (m_busy || m_closing) { if (completed) completed(false); return; }
        setBusy(true);
        m_operations->runElevatedScript(QStringLiteral("添加 Moon 并重启服务"), zerotier::moonImportScript(copies), [this, completed](bool ok) {
            setBusy(false); refresh(); feedback(ok ? QString() : QStringLiteral("文件复制或服务启动未完成，请查看本次启动日志。")); if (completed) completed(ok);
        });
    };
    if (replacements.isEmpty()) execute();
    else ask(QStringLiteral("替换 Moon 文件"), replacements.join(QStringLiteral("、")) + QStringLiteral(" 已存在，确定替换并重启服务？"), QStringLiteral("替换并重启"), execute);
}
void AppController::deleteMoon(const QString &id)
{
    if (m_busy || m_closing) return;
    const auto record = m_moons.record(id); const auto path = record["details"].toMap()["moonFile"].toString(); if (path.isEmpty()) return;
    ask(QStringLiteral("删除 Moon"), QStringLiteral("确定删除“%1”？删除后重启服务。此操作无法撤销。").arg(record["name"].toString()), QStringLiteral("删除并重启"), [this, id, path] {
        if (m_busy) return;
        setBusy(true); m_operations->runElevatedScript(QStringLiteral("删除 Moon 并重启服务"), zerotier::moonDeleteScript(path), [this, id, path](bool ok) {
            if (!QFileInfo::exists(path)) QSettings().remove("names/moons/" + id);
            setBusy(false); refresh(); feedback(ok ? QString() : QStringLiteral("文件删除或服务启动未完成，请查看本次启动日志。"));
        });
    });
}
void AppController::importPlugin(const QUrl &source)
{
    if (m_closing || !source.isLocalFile()) return;
    const auto error = plugins::importPlugin(source.toLocalFile(), m_pluginDirectory);
    if(!error.isEmpty()) { feedback(error); return; }
    const auto path=QDir(m_pluginDirectory).filePath(QFileInfo(source.toLocalFile()).fileName());
    const auto result=m_loader.loadFile(path,m_registry); feedback(result.ok()?QString():result.message);
}
void AppController::loadPlugin(const QString &id) {
    if(m_closing) return;
    for(const auto &value:m_loader.catalog()) if(value.toMap()["id"].toString()==id && value.toMap()["loadable"].toBool()) {
        const auto result=m_loader.loadFile(value.toMap()["path"].toString(),m_registry); feedback(result.ok()?QString():result.message); return;
    }
}
void AppController::unloadPlugin(const QString &id) {
    if(m_closing) return;
    if(m_activeMenu.startsWith(id+"/") && m_page) navigate(api::ZeroTierMenuId,m_page->parentItem());
    const auto result=m_loader.unload(id,m_registry); feedback(result.ok()?QString():result.message);
}
void AppController::requestQuit()
{
    if (m_closing) return;
    m_closing = true; m_confirmation = {}; updatePolling();
    for (auto context : m_memberRequests) delete context.data(); m_memberRequests.clear();
    for(auto context:m_memberProbeContexts) delete context.data(); m_memberProbeContexts.clear();
    m_shutdownMessage = QStringLiteral("正在退出，正在等待操作与插件结束…"); emit stateChanged();
    m_loader.requestShutdown();
}
void AppController::hideToTray() { if (!m_closing && m_window) emit trayRequested(); }
void AppController::restoreWindow() { if (!m_closing && m_window) platform::SingleInstance::activateWindow(*m_window); }
}
