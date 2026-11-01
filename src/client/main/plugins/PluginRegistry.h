#pragma once

#include <zto/PluginApi.h>
#include <zto/PluginLifecycle.h>
#include <QObject>
#include <QMap>
#include <QPointer>
#include <QQuickItem>
#include <QQmlEngine>

namespace vusiif::zto::plugins {

enum class PreparationState { Ready, Preparing, Failed, Stopping };

class PluginRegistry final : public QObject {
    Q_OBJECT
public:
    explicit PluginRegistry(QObject *parent = nullptr);
    ~PluginRegistry() override;

    // Registration is atomic. Definitions and their factory captures are retained.
    api::Result registerPlugin(api::PluginDefinition definition, api::PluginLifecycleInterface *lifecycle = nullptr,
                          bool preparing = false);
    api::Result unregisterPlugin(const QString &pluginId);
    QList<api::PluginInfo> plugins() const;
    QList<api::RegisteredMenu> menus() const;
    api::PageResult page(const QString &menuId, QQmlEngine *engine, QQuickItem *parent);
    api::Result setPreparationState(const QString &pluginId, PreparationState state, const QString &message = {});
    PreparationState preparationState(const QString &pluginId) const;
    QString preparationMessage(const QString &pluginId) const;
    api::Result activateMenu(const QString &menuId, const std::function<void()> &commit = {});

signals:
    void menusChanged();
    void pageCreationFailed(const QString &menuId, const QString &message);

private:
    friend class PluginLoader;
    api::Result checkMutation() const;
    QMap<QString, api::PluginDefinition> m_plugins;
    QMap<QString, QPointer<QQuickItem>> m_pages;
    QPointer<QQmlEngine> m_engine;
    QMap<QString, api::PluginLifecycleInterface *> m_lifecycles;
    api::MenuLocation m_active;
    api::MenuLocation location(const QString &menuId) const;
    void notifyTransition(const api::MenuLocation &next, const std::function<void()> &commit = {});
    struct Preparation { PreparationState state = PreparationState::Ready; QString message; };
    QMap<QString, Preparation> m_preparation;
    bool m_busy = false;
};

} // namespace vusiif::zto::plugins
