#pragma once
#include "RecordModel.h"
#include "ZeroTierClient.h"
#include "SystemOperations.h"
#include "storage/SettingsStore.h"
#include "storage/StorageContext.h"
#include "storage/StorageMaintenance.h"
#include "storage/StorageMaintenanceScheduler.h"
#include "plugins/PluginRegistry.h"
#include "plugins/PluginLoader.h"
#include "reset/ResetController.h"
#include "update/UpdateController.h"
#include <QFutureWatcher>
#include <QQuickWindow>
#include <QPointer>
#include <QSet>
#include <QUrl>
#include <memory>

namespace vusiif::zto::ui {
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
    Q_PROPERTY(QString settingsFile READ settingsFile CONSTANT)
    Q_PROPERTY(bool settingsWritable READ settingsWritable CONSTANT)
    Q_PROPERTY(QString storageRoot READ storageRoot CONSTANT)
    Q_PROPERTY(QString storageMode READ storageMode CONSTANT)
    Q_PROPERTY(QString storageWarning READ storageWarning CONSTANT)
    Q_PROPERTY(QVariantMap storageUsage READ storageUsage NOTIFY storageUsageChanged)
    Q_PROPERTY(bool storageBusy READ storageBusy NOTIFY storageUsageChanged)
    Q_PROPERTY(bool storageScanBusy READ storageScanBusy NOTIFY storageUsageChanged)
    Q_PROPERTY(QVariantList pendingDeletions READ pendingDeletions NOTIFY storageUsageChanged)
    Q_PROPERTY(QVariantList menus READ menus NOTIFY menusChanged)
    Q_PROPERTY(QVariantList plugins READ plugins NOTIFY menusChanged)
    Q_PROPERTY(QString activeMenu READ activeMenu NOTIFY activeMenuChanged)
    Q_PROPERTY(bool moonsActive READ moonsActive WRITE setMoonsActive NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap localConfig READ localConfig NOTIFY localConfigChanged)
    Q_PROPERTY(QVariantList tcpFallbackServers READ tcpFallbackServers NOTIFY tcpFallbackServersChanged)
    Q_PROPERTY(bool tcpFallbackServersBusy READ tcpFallbackServersBusy NOTIFY tcpFallbackServersChanged)
    Q_PROPERTY(QString tcpFallbackServersMessage READ tcpFallbackServersMessage NOTIFY tcpFallbackServersChanged)
    // Feature controllers own their own UI state. They are exposed as QObject
    // pointers so the settings sections only depend on the documented QML
    // contract, not on host internals.
    Q_PROPERTY(QObject *update READ updateObject CONSTANT)
    Q_PROPERTY(QObject *reset READ resetObject CONSTANT)
public:
    explicit AppController(QObject *parent = nullptr, const QString &pluginDirectory = {},
                           SystemOperations *operations = nullptr,
                           const storage::StorageContext &storage = storage::StorageContext::resolve());
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
    QString settingsFile() const { return m_settings.fileName(); }
    // False while the shared-root migration still has to restore settings.ini.
    // QML persistence must go through hostSetting()/setHostSetting() so this
    // gate applies to window, sidebar and column state as well.
    bool settingsWritable() const { return m_settings.writeProtection().isEmpty(); }
    Q_INVOKABLE QVariant hostSetting(const QString &key, const QVariant &fallback = {}) const {
        return m_settings.value(key, fallback);
    }
    Q_INVOKABLE void setHostSetting(const QString &key, const QVariant &value);
    QString storageRoot() const { return m_storage.activeRoot(); }
    QString storageMode() const;
    QString storageWarning() const;
    QVariantMap storageUsage() const { return m_storageUsage; }
    bool storageBusy() const { return m_storageBusy; }
    bool storageScanBusy() const { return m_storageScanBusy; }
    QVariantList pendingDeletions() const { return m_loader.pendingDeletions(); }
    Q_INVOKABLE void refreshStorageUsage();
    Q_INVOKABLE void cleanupStorage(const QString &category);
    Q_INVOKABLE QVariantList storageOrphans() const;
    storage::StorageContext &storageContext() { return m_storage; }
    storage::SettingsStore &settings() { return m_settings; }
    QVariantList menus() const;
    QVariantList plugins() const;
    QString activeMenu() const { return m_activeMenu; }
    bool moonsActive() const { return m_moonsActive; }
    void setMoonsActive(bool active);
    QVariantMap localConfig() const;
    QObject *updateObject() { return m_update.get(); }
    QObject *resetObject() { return m_reset.get(); }
    QVariantList tcpFallbackServers() const { return m_tcpFallbackServers; }
    bool tcpFallbackServersBusy() const { return m_tcpFallbackServersBusy; }
    QString tcpFallbackServersMessage() const { return m_tcpFallbackServersMessage; }
    Q_INVOKABLE void refreshLocalConfig();
    Q_INVOKABLE void retryLocalConfig();
    Q_INVOKABLE void setTcpFallbackSwitches(bool allow, bool force);
    Q_INVOKABLE void refreshTcpFallbackServers();
    Q_INVOKABLE void selectTcpFallbackServer(const QString &networkId);
    Q_INVOKABLE void revealLocalConfig();
    Q_INVOKABLE void revealSettings();
    Q_INVOKABLE void openLogs();
    Q_PROPERTY(QString tcpFallbackNetworkName READ tcpFallbackNetworkName NOTIFY tcpFallbackServersChanged)
    QString tcpFallbackNetworkName() const;
    Q_PROPERTY(bool peersExpanded READ peersExpanded WRITE setPeersExpanded NOTIFY stateChanged)
    bool peersExpanded() const { return m_peersExpanded; }
    void setPeersExpanded(bool expanded);
    Q_INVOKABLE bool copyNodeId();
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
    void storageUsageChanged();
    void storageScanFinished();
    void localConfigChanged();
    void tcpFallbackServersChanged();
    void confirmationRequested(const QString &title, const QString &message, const QString &acceptLabel);
    void pluginNoticeRequested(const QString &title, const QString &message, bool error = true);
    void notificationRequested(const QString &message, bool error);
    void networkJoined();
    void configurationImported(const QString &networkId);
    void exitFinished();
    void trayRequested();
    void nodeIdCopied();
private:
    storage::StorageContext m_storage;
    storage::SettingsStore m_settings;
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
    zerotier::localconfig::State m_localConfig;
    QVariantList m_tcpFallbackServers;
    QString m_localConfigPath, m_localConfigError, m_tcpFallbackServersMessage;
    bool m_localConfigLoaded = false, m_localConfigBusy = false;
    bool m_tcpFallbackServersBusy = false;
    zerotier::ServiceStatus m_serviceStatus;
    QString m_pluginDirectory, m_feedback, m_networkStatus, m_peerStatus, m_shutdownMessage, m_storageWarning;
    QVariantMap m_storageUsage;
    QVariantMap m_storageMaintenance;
    struct StorageScanResult {
        storage::StorageUsage usage;
        storage::RetentionPreview preview;
        QList<storage::OrphanItem> orphans;
        bool uncertainPackages = false;
    };
    QFutureWatcher<StorageScanResult> *m_storageScanWatcher = nullptr;
    QVariantList m_storageOrphans;
    std::unique_ptr<storage::StorageMaintenanceScheduler> m_maintenance;
    bool m_storageBusy = false;
    bool m_storageScanBusy = false;
    bool m_storageScanQueued = false;
    bool m_storageScanComplete = false;
    QString m_storageScanError;
    bool m_settingsWriteReported = false;
    struct PendingStorageRequest {
        bool valid = false;
        QString category;
        QStringList ids;
        bool confirmed = false;
    } m_pendingStorageRequest;
    void handleStorageScanFinished();
    QStringList installedPluginIds() const;
    QString maintenanceText() const;
    void runStorageCleanup(const QString &category, const QStringList &ids);
    void requestStorageScan();
    QString m_activeMenu = QString::fromLatin1(api::ZeroTierMenuId);
    bool m_busy = false, m_closing = false, m_shutdownReady = false, m_visible = false, m_moonsActive = false, m_apiAvailable = false;
    bool m_serviceInterrupted = false, m_peersExpanded = false;
    quint64 m_serviceGeneration = 0;
    bool m_serviceQueryPending = false, m_moonFilesPending = false, m_moonRefreshQueued = false;
    QHash<QString, QPointer<QObject>> m_memberRequests;
    QHash<QString, QPointer<QObject>> m_memberProbeContexts;
    std::function<void()> m_confirmation;
    std::function<void(bool)> m_serviceMonitor;
    std::unique_ptr<update::UpdateController> m_update;
    std::unique_ptr<reset::ResetController> m_reset;
    bool m_updateStartupChecked = false;
    bool m_resetConfirmationPending = false;
    void connectFeatureControllers();
    void scheduleStartupUpdateCheck();
    void ask(const QString &title, const QString &message, const QString &acceptLabel, std::function<void()> action);
    void setBusy(bool busy);
    void feedback(const QString &message, bool error = true);
    void updatePolling();
    void updateService();
    void beginServiceInterruption();
    void endServiceInterruption();
    void refreshMembers();
    void refreshMoonFiles();
    QString localName(const QString &kind, const QString &id, const QString &fallback = {}) const;
    void networkCommand(const QString &action, const QString &id, bool moonConfigured = false);
    void importMoonFiles(const QStringList &sources, std::function<void(bool)> completed = {},
                         const QString &tcpFallbackRelay = {});
    void loadLocalConfig(bool allowElevation);
    void beginElevatedLocalConfigRead();
    void applyLocalConfig(const zerotier::localconfig::State &state, const QString &error);
    void recoverLocalConfig(const QString &error);
    bool currentLocalConfigForUpdate(zerotier::localconfig::State &state, QString &expectedHash, QString &error);
    void writeLocalConfig(const QByteArray &content, const QString &expectedHash,
                          std::function<void(bool, const QString &)> finished);
    void applyTcpFallbackRelay(const QString &networkId, const QString &ip);
    bool prepareImportRelay(const QString &display, const QString &directoryPath,
                            zerotier::localconfig::Install &install, QByteArray &content, QString &error);
    QVariantMap details(const QString &kind, const QString &id) const;
};
}
