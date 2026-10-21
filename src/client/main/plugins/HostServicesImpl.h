#pragma once

#include <ztgui/HostServices.h>
#include <ztgui/PluginStorage.h>
#include "ServiceBroker.h"
#include "storage/PluginDataStore.h"
#include <QObject>
#include <QFile>
#include <QLockFile>
#include <memory>

namespace vusiif::ztgui::util {
// Host implementation of the borrowed service objects. The stable plugin ID
// selects plugins-data/id-<normalized-id>; the display name is only metadata.
class HostServicesImpl final : public QObject, public HostServices,
                               public PluginFiles, public ZeroTierService, public PluginLogger,
                               public ServiceAccess, public PluginStorage {
public:
    explicit HostServicesImpl(const QString &pluginName, const QString &dataRoot = {},
                              const QString &pluginId = {}, const QString &metadataRoot = {},
                              const QString &runId = {}, const QString &userKey = {},
                              std::function<void(const QString &)> cacheWriteNotifier = {},
                              const storage::PluginDataStore::InstalledNameClaims &nameClaims = {});
    ~HostServicesImpl() override;
    PluginFiles &files() override { return *this; }
    ZeroTierService &zeroTier() override { return *this; }
    PluginLogger &logs() override { return *this; }
    PluginServices &pluginServices() override { return serviceClient; }
    plugins::ServiceClient serviceClient;
    LogResult write(LogLevel level, const QString &message) override;
    FileResult readFile(const QString &relativePath) override;
    FileResult writeFile(const QString &relativePath, const QByteArray &data) override;
    PluginFileTransaction updateFile(const QString &relativePath,
                                     const PluginFileUpdater &updater) override;
    LegacyImportState legacyImportAllowed() override;
    StorageResult completeLegacyImport() override;
    void getZTInfo(QObject *context, JsonCallback completed) override;
    void getZTPeers(QObject *context, JsonCallback completed) override;
    void getZTNetworks(QObject *context, JsonCallback completed) override;
    void getZTMoons(QObject *context, JsonCallback completed) override;
    void sendDataTo(const QString &, const QString &, const QByteArray &, QObject *, SendCallback) override;
    void broadcastDataTo(const QString &, const QStringList &, const QByteArray &, QObject *, SendCallback) override;
    void broadcastDataBalancedTo(const QString &, const QStringList &, const QByteArray &, QObject *, SendCallback) override;
    void getNetworkRelays(const QString &, QObject *, RelayCallback) override;
    NetworkSubscription *subscribeNetworkData(const QString &, QObject *, NetworkCallback) override;
    void stopRequests();
    bool hasCallbacks() const { return m_callbacks.use_count()>1; }
    // False when the stable-ID data directory or host metadata could not be
    // prepared. The loader must reject the plugin instead of silently falling
    // back to a display-name directory.
    bool storageReady() const { return m_storageError.isEmpty(); }
    QString storageError() const { return m_storageError; }
    QString dataDirectory() const { return m_dataRoot; }
    // Non-empty while a pending shared-root migration still owns this plugin's
    // persistent data directory; persistent writes are refused until it settles.
    QString migrationBlock() const { return m_migrationBlock; }
    QStringList storageWarnings() const { return m_store.warnings(); }
private:
    QString m_sharedDataRoot;
    QString m_pluginName;
    QString m_dataRoot;
    QString m_pluginId;
    QString m_storageError;
    QString m_migrationBlock;
    std::function<void(const QString &)> m_cacheWriteNotifier;
    storage::PluginDataStore m_store;
    QFile m_logFile;
    QObject m_requests;
    bool m_stopped = false;
    std::shared_ptr<int> m_callbacks=std::make_shared<int>(0);
    QString filePath(const QString &relativePath, bool writing, QString &error) const;
    bool acquireFileActivityLock(const QString &path, std::unique_ptr<QLockFile> &lock,
                                 QString &error, int timeoutMs) const;
    void notifyCacheWrite(const QString &relativePath);
    void query(const QString &command, bool object, QObject *context, JsonCallback completed);
    void sendNetwork(const QString &, const QStringList &, const QByteArray &, bool, QObject *, SendCallback);
};
}
