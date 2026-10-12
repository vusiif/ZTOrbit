#pragma once

#include <ztgui/HostServices.h>
#include <QObject>
#include <QFile>

namespace vusiif::ztgui::util {
class HostServicesImpl final : public QObject, public HostServices,
                               public PluginFiles, public ZeroTierService, public PluginLogger {
public:
    explicit HostServicesImpl(const QString &pluginName, const QString &dataRoot = {}, const QString &pluginId = {});
    ~HostServicesImpl() override;
    PluginFiles &files() override { return *this; }
    ZeroTierService &zeroTier() override { return *this; }
    PluginLogger &logs() override { return *this; }
    LogResult write(LogLevel level, const QString &message) override;
    FileResult readFile(const QString &relativePath) override;
    FileResult writeFile(const QString &relativePath, const QByteArray &data) override;
    void getZTInfo(QObject *context, JsonCallback completed) override;
    void getZTPeers(QObject *context, JsonCallback completed) override;
    void getZTNetworks(QObject *context, JsonCallback completed) override;
    void getZTMoons(QObject *context, JsonCallback completed) override;
    void sendDataTo(const QString &, const QString &, const QByteArray &, QObject *, SendCallback) override;
    void broadcastDataTo(const QString &, const QByteArray &, QObject *, SendCallback) override;
    void stopRequests();
private:
    QString m_sharedDataRoot;
    QString m_pluginName;
    QString m_dataRoot;
    QString m_pluginId;
    QFile m_logFile;
    QObject m_requests;
    bool m_stopped = false;
    QString filePath(const QString &relativePath, bool writing, QString &error) const;
    void query(const QString &command, bool object, QObject *context, JsonCallback completed);
    void sendNetwork(const QString &, const QString &, const QByteArray &, QObject *, SendCallback);
};
}
