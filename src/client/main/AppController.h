#pragma once
#include "RecordModel.h"
#include "ZeroTierClient.h"
#include "SystemOperations.h"
#include "plugins/PluginRegistry.h"
#include "plugins/PluginLoader.h"
#include <QQuickWindow>
#include <QPointer>
#include <QSet>

namespace vusiif::ztgui::ui {
class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(RecordModel *networks READ networks CONSTANT)
    Q_PROPERTY(RecordModel *peers READ peers CONSTANT)
    Q_PROPERTY(RecordModel *moons READ moons CONSTANT)
    Q_PROPERTY(QVariantMap info READ info NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap service READ service NOTIFY stateChanged)
    Q_PROPERTY(QString networkStatus READ networkStatus NOTIFY stateChanged)
    Q_PROPERTY(QString peerStatus READ peerStatus NOTIFY stateChanged)
    Q_PROPERTY(QString feedback READ feedback NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool closing READ closing NOTIFY stateChanged)
    Q_PROPERTY(QString shutdownMessage READ shutdownMessage NOTIFY stateChanged)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
    Q_PROPERTY(bool dark READ dark NOTIFY themeChanged)
    Q_PROPERTY(bool disableGpuAcceleration READ disableGpuAcceleration WRITE setDisableGpuAcceleration NOTIFY disableGpuAccelerationChanged)
    Q_PROPERTY(bool gpuAccelerationForcedOff READ gpuAccelerationForcedOff CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString apiVersion READ apiVersion CONSTANT)
    Q_PROPERTY(QString serverVersion READ serverVersion CONSTANT)
    Q_PROPERTY(QString serverApiVersion READ serverApiVersion CONSTANT)
    Q_PROPERTY(QString qtVersion READ qtVersion CONSTANT)
    Q_PROPERTY(QVariantList menus READ menus NOTIFY menusChanged)
    Q_PROPERTY(QVariantList plugins READ plugins NOTIFY menusChanged)
    Q_PROPERTY(QString activeMenu READ activeMenu NOTIFY activeMenuChanged)
    Q_PROPERTY(bool moonsActive READ moonsActive WRITE setMoonsActive NOTIFY stateChanged)
public:
    explicit AppController(QObject *parent = nullptr, const QString &pluginDirectory = {}, SystemOperations *operations = nullptr);
    ~AppController() override;
    RecordModel *networks() { return &m_networks; }
    RecordModel *peers() { return &m_peers; }
    RecordModel *moons() { return &m_moons; }
    QVariantMap info() const { return m_info; }
    QVariantMap service() const;
    QString networkStatus() const { return m_networkStatus; }
    QString peerStatus() const { return m_peerStatus; }
    QString feedback() const { return m_feedback; }
    bool busy() const { return m_busy; }
    bool closing() const { return m_closing; }
    QString shutdownMessage() const { return m_shutdownMessage; }
    QString theme() const;
    void setTheme(const QString &theme);
    bool dark() const;
    bool disableGpuAcceleration() const;
    void setDisableGpuAcceleration(bool disabled);
    bool gpuAccelerationForcedOff() const;
    QString version() const;
    QString apiVersion() const;
    QString serverVersion() const;
    QString serverApiVersion() const;
    QString qtVersion() const;
    QVariantList menus() const;
    QVariantList plugins() const;
    QString activeMenu() const { return m_activeMenu; }
    bool moonsActive() const { return m_moonsActive; }
    void setMoonsActive(bool active);
    void setWindow(QQuickWindow *window);
    void setVisible(bool visible);
    plugins::PluginRegistry *pluginRegistry() { return &m_registry; }
    Q_INVOKABLE bool navigate(const QString &menuId, QQuickItem *container);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void serviceAction(const QString &action);
    Q_INVOKABLE void packageAction(bool uninstall);
    Q_INVOKABLE void joinNetwork(const QString &id);
    Q_INVOKABLE void leaveNetwork(const QString &id);
    Q_INVOKABLE void setExpanded(const QString &id, bool expanded);
    Q_INVOKABLE void rename(const QString &kind, const QString &id, const QString &name);
    Q_INVOKABLE void copyText(const QString &text);
    Q_INVOKABLE QString detailsJson(const QString &kind, const QString &id) const;
    Q_INVOKABLE QVariantList detailFields(const QString &kind, const QString &id) const;
    Q_INVOKABLE bool hasMoon(const QString &networkId) const;
    Q_INVOKABLE void exportConfiguration(const QString &networkId, const QUrl &destination);
    Q_INVOKABLE void importConfiguration(const QUrl &source);
    Q_INVOKABLE void importMoons(const QList<QUrl> &sources);
    Q_INVOKABLE void deleteMoon(const QString &id);
    Q_INVOKABLE void loadPlugin(const QString &id);
    Q_INVOKABLE void unloadPlugin(const QString &id);
    Q_INVOKABLE void deletePlugin(const QString &path);
    Q_INVOKABLE void importPlugin(const QUrl &source);
    Q_INVOKABLE void confirm(bool accepted);
    Q_INVOKABLE void requestQuit();
    Q_INVOKABLE void hideToTray();
    Q_INVOKABLE void restoreWindow();
signals:
    void stateChanged();
    void themeChanged();
    void disableGpuAccelerationChanged();
    void menusChanged();
    void activeMenuChanged();
    void confirmationRequested(const QString &title, const QString &message, const QString &acceptLabel);
    void pluginNoticeRequested(const QString &title, const QString &message);
    void networkJoined();
    void configurationImported(const QString &networkId);
    void exitFinished();
    void trayRequested();
private:
    RecordModel m_networks, m_peers, m_moons;
    plugins::PluginRegistry m_registry;
    plugins::PluginLoader m_loader;
    SystemOperations *m_operations;
    zerotier::JsonPoller *m_infoPoller, *m_networkPoller, *m_peerPoller, *m_moonPoller;
    QTimer m_statusTimer, m_memberTimer;
    QPointer<QQuickWindow> m_window;
    WId m_windowId = 0;
    QPointer<QQuickItem> m_page;
    QVariantMap m_info;
    QJsonDocument m_moonMetadata;
    zerotier::ServiceStatus m_serviceStatus;
    QString m_pluginDirectory, m_feedback, m_networkStatus, m_peerStatus, m_shutdownMessage;
    QString m_activeMenu = QString::fromLatin1(api::ZeroTierMenuId);
    bool m_busy = false, m_closing = false, m_shutdownReady = false, m_visible = false, m_moonsActive = false, m_apiAvailable = false;
    bool m_serviceQueryPending = false, m_moonFilesPending = false, m_moonRefreshQueued = false;
    QHash<QString, QPointer<QObject>> m_memberRequests;
    QHash<QString, QPointer<QObject>> m_memberProbeContexts;
    std::function<void()> m_confirmation;
    std::function<void(bool)> m_serviceMonitor;
    void ask(const QString &title, const QString &message, const QString &acceptLabel, std::function<void()> action);
    void setBusy(bool busy);
    void feedback(const QString &message);
    void updatePolling();
    void updateService();
    void refreshMembers();
    void refreshMoonFiles();
    void networkCommand(const QString &action, const QString &id, bool moonConfigured = false);
    void importMoonFiles(const QStringList &sources, std::function<void(bool)> completed = {});
    QVariantMap details(const QString &kind, const QString &id) const;
};
}
