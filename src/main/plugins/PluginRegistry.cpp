#include "PluginRegistry.h"

#include <QApplication>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <exception>
#include <utility>

namespace ZtGui::Plugins {
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

PluginRegistry::PluginRegistry(QObject *parent) : QObject(parent) {}

PluginRegistry::~PluginRegistry()
{
    const QScopedValueRollback<bool> guard(m_busy, true);
    // Destroy widgets while factory captures (and future loader handles) still live.
    for (const auto &page : std::as_const(m_pages)) delete page.data();
    m_pages.clear();
    m_plugins.clear();
}

Result PluginRegistry::checkMutation() const
{
    if (!qApp || QThread::currentThread() != qApp->thread() || thread() != qApp->thread())
        return {Error::WrongThread, QStringLiteral("插件 API 必须在 GUI 线程调用")};
    if (m_busy) return {Error::Busy, QStringLiteral("插件 API 正在创建或释放页面")};
    return {};
}

Result PluginRegistry::registerPlugin(PluginDefinition definition)
{
    if (const auto result = checkMutation(); !result.ok()) return result;
    const auto &info = definition.info;
    if (!validId(info.id) || info.id == "builtin" || info.name.trimmed().isEmpty()
        || info.version.trimmed().isEmpty())
        return {Error::InvalidPlugin, QStringLiteral("插件 ID、名称或版本无效")};
    if (!isApiCompatible(info.apiMajor, info.apiMinor))
        return {Error::IncompatibleApi, QStringLiteral("插件要求的 API 版本不兼容")};
    if (m_plugins.contains(info.id))
        return {Error::DuplicatePlugin, QStringLiteral("插件 ID 已注册：%1").arg(info.id)};
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
        for (const auto &menu : plugin->menus) {
            auto page = m_pages.take(fullId(pluginId, menu.id));
            delete page.data();
        }
        m_plugins.erase(plugin);
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

PageResult PluginRegistry::page(const QString &menuId, QWidget *parent)
{
    if (const auto result = checkMutation(); !result.ok()) return {nullptr, result};
    if (!parent) return {nullptr, {Error::PageCreationFailed, QStringLiteral("页面需要宿主父控件")}};
    const auto separator = menuId.indexOf('/');
    const auto plugin = m_plugins.constFind(menuId.left(separator));
    if (separator < 0 || plugin == m_plugins.cend())
        return {nullptr, {Error::NotFound, QStringLiteral("菜单未注册：%1").arg(menuId)}};
    const auto menu = std::find_if(plugin->menus.cbegin(), plugin->menus.cend(), [&](const MenuContribution &item) {
        return fullId(plugin->info.id, item.id) == menuId;
    });
    if (menu == plugin->menus.cend())
        return {nullptr, {Error::NotFound, QStringLiteral("菜单未注册：%1").arg(menuId)}};
    if (auto *cached = m_pages.value(menuId).data()) {
        if (cached->parentWidget() != parent)
            return {nullptr, {Error::PageCreationFailed, QStringLiteral("页面已属于另一个宿主")}};
        return {cached, {}};
    }

    const auto existingChildren = parent->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    QWidget *widget = nullptr;
    QString failure;
    bool sharedWidget = false;
    {
        const QScopedValueRollback<bool> guard(m_busy, true);
        try {
            widget = menu->createPage(parent);
            if (!widget) failure = QStringLiteral("页面工厂未返回控件");
            else if (widget == parent || widget->parentWidget() != parent)
                failure = QStringLiteral("页面工厂必须返回由宿主父控件持有的新控件");
            else if (existingChildren.contains(widget))
                failure = QStringLiteral("页面工厂不能借用宿主已有的控件");
            else {
                for (const auto &existing : std::as_const(m_pages)) {
                    if (existing == widget) {
                        sharedWidget = true;
                        failure = QStringLiteral("不同菜单不能共用页面控件");
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
            && widget->parentWidget() == parent) delete widget;
    }
    if (!failure.isEmpty()) {
        emit pageCreationFailed(menuId, failure);
        return {nullptr, {Error::PageCreationFailed, failure}};
    }
    return {widget, {}};
}

} // namespace ZtGui::Plugins
