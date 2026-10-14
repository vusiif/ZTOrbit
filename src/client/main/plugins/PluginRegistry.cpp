#include "PluginRegistry.h"
#include <ztprotocol/NetworkDelivery.h>

#include <QGuiApplication>
#include <QDebug>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <exception>
#include <utility>

namespace vusiif::ztgui::plugins {
using namespace vusiif::ztgui::api;
namespace {
bool validId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[a-z0-9][a-z0-9._-]{0,127}\\z"));
    return pattern.match(id).hasMatch();
}

QString fullId(const QString &pluginId, const QString &menuId)
{
    return pluginId + '/' + menuId;
}
}

PluginRegistry::PluginRegistry(QObject *parent) : QObject(parent), m_active(location(ZeroTierMenuId)) {}

PluginRegistry::~PluginRegistry()
{
    const QScopedValueRollback<bool> guard(m_busy, true);
    notifyTransition(location(ZeroTierMenuId));
    // Destroy Quick items while factory captures (and future loader handles) still live.
    for (const auto &page : std::as_const(m_pages)) delete page.data();
    m_pages.clear();
    m_plugins.clear();
    if (m_engine) { m_engine->collectGarbage(); m_engine->clearComponentCache(); }
}

Result PluginRegistry::checkMutation() const
{
    if (!qApp || QThread::currentThread() != qApp->thread() || thread() != qApp->thread())
        return {Error::WrongThread, QStringLiteral("插件 API 必须在 GUI 线程调用")};
    if (m_busy) return {Error::Busy, QStringLiteral("插件 API 正在创建或释放页面")};
    return {};
}

Result PluginRegistry::registerPlugin(PluginDefinition definition, PluginLifecycleInterface *lifecycle, bool preparing)
{
    if (const auto result = checkMutation(); !result.ok()) return result;
    const auto &info = definition.info;
    if (!protocol::validPluginId(info.id) || info.id == "builtin" || info.name.trimmed().isEmpty()
        || info.version.trimmed().isEmpty())
        return {Error::InvalidPlugin, QStringLiteral("插件 ID、名称或版本无效")};
    if (!isApiCompatible(info.apiMajor, info.apiMinor))
        return {Error::IncompatibleApi, QStringLiteral("插件要求的 API 版本不兼容")};
    if (std::any_of(m_plugins.cbegin(),m_plugins.cend(),[&](const api::PluginDefinition &p) { return p.info.id.compare(info.id,Qt::CaseInsensitive)==0; }))
        return {Error::DuplicatePlugin, QStringLiteral("插件 ID 已注册：%1").arg(info.id)};
    for(const auto &registered:m_plugins) if(registered.info.name.compare(info.name,Qt::CaseInsensitive)==0)
        return {Error::DuplicatePlugin,QStringLiteral("插件显示名对应的数据目录已被使用")};
    if (definition.menus.isEmpty())
        return {Error::InvalidMenu, QStringLiteral("插件至少需要声明一个菜单")};
    QSet<QString> ids;
    for (const auto &menu : definition.menus) {
        if (!validId(menu.id) || ids.contains(menu.id) || menu.title.trimmed().isEmpty() || !menu.createPage)
            return {Error::InvalidMenu, QStringLiteral("菜单 ID 重复、格式无效或缺少标题／页面工厂：%1").arg(menu.id)};
        ids.insert(menu.id);
    }
    const QString id = info.id;
    m_plugins.insert(id, std::move(definition));
    if (lifecycle) m_lifecycles.insert(id, lifecycle);
    if (preparing) m_preparation.insert(id, {PreparationState::Preparing, QStringLiteral("插件准备中…")});
    emit menusChanged();
    return {};
}

Result PluginRegistry::unregisterPlugin(const QString &pluginId)
{
    if (const auto result = checkMutation(); !result.ok()) return result;
    const auto plugin = m_plugins.find(pluginId);
    if (plugin == m_plugins.end()) return {Error::NotFound, QStringLiteral("插件未注册：%1").arg(pluginId)};
    {
        const QScopedValueRollback<bool> guard(m_busy, true);
        if (m_active.plugin.id == pluginId) notifyTransition(location(ZeroTierMenuId));
        for (const auto &menu : plugin->menus) {
            auto page = m_pages.take(fullId(pluginId, menu.id));
            delete page.data();
        }
        m_plugins.erase(plugin);
        m_lifecycles.remove(pluginId);
        m_preparation.remove(pluginId);
        if (m_engine) { m_engine->collectGarbage(); m_engine->clearComponentCache(); }
    }
    emit menusChanged();
    return {};
}

QList<PluginInfo> PluginRegistry::plugins() const
{
    Q_ASSERT(QThread::currentThread() == thread());
    QList<PluginInfo> result;
    for (const auto &plugin : m_plugins) result.append(plugin.info);
    return result;
}

QList<RegisteredMenu> PluginRegistry::menus() const
{
    Q_ASSERT(QThread::currentThread() == thread());
    QList<RegisteredMenu> result;
    for (const auto &plugin : m_plugins) {
        for (const auto &menu : plugin.menus)
            result.append({fullId(plugin.info.id, menu.id), plugin.info.id, menu.id,
                           menu.title, menu.icon, menu.toolTip, menu.order});
    }
    std::sort(result.begin(), result.end(), [](const RegisteredMenu &a, const RegisteredMenu &b) {
        return a.order != b.order ? a.order < b.order : a.id < b.id;
    });
    return result;
}

PageResult PluginRegistry::page(const QString &menuId, QQmlEngine *engine, QQuickItem *parent)
{
    if (const auto result = checkMutation(); !result.ok()) return {nullptr, result};
    if (!engine || !parent) return {nullptr, {Error::PageCreationFailed, QStringLiteral("页面需要宿主引擎和父项")}};
    if (m_engine && m_engine != engine) return {nullptr, {Error::PageCreationFailed, QStringLiteral("页面引擎不能改变")}};
    m_engine = engine;
    const auto separator = menuId.indexOf('/');
    const auto plugin = m_plugins.constFind(menuId.left(separator));
    if (separator < 0 || plugin == m_plugins.cend())
        return {nullptr, {Error::NotFound, QStringLiteral("菜单未注册：%1").arg(menuId)}};
    const auto preparation = m_preparation.value(plugin->info.id);
    if (preparation.state != PreparationState::Ready)
        return {nullptr, {preparation.state == PreparationState::Preparing ? Error::Busy : Error::PageCreationFailed,
            preparation.message}};
    const auto menu = std::find_if(plugin->menus.cbegin(), plugin->menus.cend(), [&](const MenuContribution &item) {
        return fullId(plugin->info.id, item.id) == menuId;
    });
    if (menu == plugin->menus.cend())
        return {nullptr, {Error::NotFound, QStringLiteral("菜单未注册：%1").arg(menuId)}};
    if (auto *cached = m_pages.value(menuId).data()) {
        if (cached->parentItem() != parent)
            return {nullptr, {Error::PageCreationFailed, QStringLiteral("页面已属于另一个宿主")}};
        return {cached, {}};
    }

    const auto existingChildren = parent->findChildren<QQuickItem *>(QString(), Qt::FindDirectChildrenOnly);
    QQuickItem *widget = nullptr;
    QString failure;
    bool sharedWidget = false;
    {
        const QScopedValueRollback<bool> guard(m_busy, true);
        try {
            widget = menu->createPage(engine, parent);
            if (!widget) failure = QStringLiteral("页面工厂未返回项");
            else if (widget == parent || (widget->parentItem() != parent || widget->parent() != parent))
                failure = QStringLiteral("页面工厂必须返回由宿主父项持有的新项");
            else if (existingChildren.contains(widget))
                failure = QStringLiteral("页面工厂不能借用宿主已有的项");
            else {
                for (const auto &existing : std::as_const(m_pages)) {
                    if (existing == widget) {
                        sharedWidget = true;
                        failure = QStringLiteral("不同菜单不能共用页面项");
                        break;
                    }
                }
            }
        } catch (const std::exception &error) {
            failure = QString::fromUtf8(error.what());
        } catch (...) {
            failure = QStringLiteral("页面工厂抛出未知异常");
        }
        if (failure.isEmpty()) m_pages.insert(menuId, widget);
        if (!failure.isEmpty() && widget && widget != parent && !sharedWidget && !existingChildren.contains(widget)
            && widget->parentItem() == parent) delete widget;
    }
    if (!failure.isEmpty()) {
        emit pageCreationFailed(menuId, failure);
        return {nullptr, {Error::PageCreationFailed, failure}};
    }
    return {widget, {}};
}

Result PluginRegistry::setPreparationState(const QString &pluginId, PreparationState state, const QString &message)
{
    if (const auto result = checkMutation(); !result.ok()) return result;
    if (!m_plugins.contains(pluginId)) return {Error::NotFound, QStringLiteral("插件未注册：%1").arg(pluginId)};
    m_preparation.insert(pluginId, {state, message});
    emit menusChanged();
    return {};
}

PreparationState PluginRegistry::preparationState(const QString &pluginId) const
{
    Q_ASSERT(QThread::currentThread() == thread());
    return m_preparation.value(pluginId).state;
}

QString PluginRegistry::preparationMessage(const QString &pluginId) const
{
    Q_ASSERT(QThread::currentThread() == thread());
    return m_preparation.value(pluginId).message;
}

MenuLocation PluginRegistry::location(const QString &menuId) const
{
    if (menuId == QLatin1String(ZeroTierMenuId))
        return {{"builtin", QStringLiteral("ZeroTier"), QCoreApplication::applicationVersion()},
                {ZeroTierMenuId, "builtin", "zerotier", QStringLiteral("ZeroTier"), {}, {}, 0}, true};
    if (menuId == QLatin1String(SettingsMenuId))
        return {{"builtin", QStringLiteral("设置"), QCoreApplication::applicationVersion()},
                {SettingsMenuId,"builtin","settings",QStringLiteral("设置"),{},{},0},true};
    for (const auto &menu : menus()) {
        if (menu.id == menuId) return {m_plugins.value(menu.pluginId).info, menu, false};
    }
    return {};
}

void PluginRegistry::notifyTransition(const MenuLocation &next, const std::function<void()> &commit)
{
    if (m_active.menu.id == next.menu.id) { if (commit) commit(); return; }
    const auto previous = m_active;
    auto invoke = [](auto callback) {
        try { callback(); }
        catch (const std::exception &e) { qWarning() << "[插件] 导航生命周期异常:" << e.what(); }
        catch (...) { qWarning() << "[插件] 导航生命周期抛出未知异常"; }
    };
    if (auto *old = m_lifecycles.value(previous.plugin.id))
        invoke([&] { old->onExitedTo(next); });
    m_active = next;
    if (commit) commit();
    if (auto *entry = m_lifecycles.value(next.plugin.id))
        invoke([&] { entry->onEnteredFrom(previous); });
}

Result PluginRegistry::activateMenu(const QString &menuId, const std::function<void()> &commit)
{
    if (const auto result = checkMutation(); !result.ok()) return result;
    const auto next = location(menuId);
    if (next.menu.id.isEmpty()) return {Error::NotFound, QStringLiteral("菜单未注册：%1").arg(menuId)};
    if (!next.builtin && preparationState(next.plugin.id) != PreparationState::Ready)
        return {Error::Busy, QStringLiteral("插件尚未准备就绪")};
    const QScopedValueRollback<bool> guard(m_busy, true);
    notifyTransition(next, commit);
    return {};
}

} // namespace vusiif::ztgui::plugins
