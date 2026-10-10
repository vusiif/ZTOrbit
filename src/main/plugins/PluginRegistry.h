#pragma once

#include <ztgui/PluginApi.h>
#include <QObject>
#include <QMap>
#include <QPointer>
#include <QWidget>

namespace ZtGui::Plugins {

class PluginRegistry final : public QObject {
    Q_OBJECT
public:
    explicit PluginRegistry(QObject *parent = nullptr);
    ~PluginRegistry() override;

    // Registration is atomic. Definitions and their factory captures are retained.
    Result registerPlugin(PluginDefinition definition);
    Result unregisterPlugin(const QString &pluginId);
    QList<PluginInfo> plugins() const;
    QList<RegisteredMenu> menus() const;
    PageResult page(const QString &menuId, QWidget *parent);

signals:
    void menusChanged();
    void pageCreationFailed(const QString &menuId, const QString &message);

private:
    Result checkMutation() const;
    QMap<QString, PluginDefinition> m_plugins;
    QMap<QString, QPointer<QWidget>> m_pages;
    bool m_busy = false;
};

} // namespace ZtGui::Plugins
