#pragma once
#include "LocalConfig.h"
#include <QElapsedTimer>

#include <QObject>
#include <QJsonDocument>
#include <QProcess>
#include <QTimer>
#include <functional>
#include <memory>
#include <QList>
#include <QPair>

class QTemporaryDir;

namespace vusiif::zto::zerotier {
struct ServiceStatus { bool exists = false; bool running = false; bool known = true; };
ServiceStatus serviceStatus();
QString matchingMoonFile(const QString &networkId);
QString executable();
QString dataDirectory();
QString homeDirectory();
QString systemHomeDirectory();
QString moonDirectory();
QString moonWriteDirectory();
QString localConfigPath();
QString wrapScript(const QString &commands);
QString moonImportScript(const QList<QPair<QString, QString>> &files,
                         const localconfig::Install *localConfig = nullptr);
QString moonDeleteScript(const QString &path);
QString sessionRefreshScript();
std::function<void(bool)> createServiceMonitor(QObject *owner);
QString translate(const QString &value);
QString jsonText(const QJsonValue &value);
QString quotePowerShell(const QString &value);
QString serviceCommands(const QString &action);
QString packageScript(bool uninstall, const QString &winget);
struct PowerShellInvocation {
    std::shared_ptr<QTemporaryDir> directory;
    QString logPath;
    QStringList arguments;
};
PowerShellInvocation preparePowerShell(const QString &script);
void command(QObject *owner, const QStringList &arguments,
             std::function<void(bool, const QString &)> finished);

// Each poll owns one process. Slow requests never overlap or block the UI.
class JsonPoller : public QObject {
    Q_OBJECT
public:
    JsonPoller(QObject *owner, const QString &command, bool object = false);
    ~JsonPoller() override;
    void refresh();
    void setActive(bool active);
    bool isActive() const { return m_active; }
signals:
    void updated(const QJsonDocument &document);
    void availabilityChanged(bool available, const QString &message);
private:
    bool m_active = false;
    QString m_command;
    bool m_object;
    QProcess m_process;
    QTimer m_interval;
    QTimer m_timeout;
    QString m_lastFailure;
    int m_failures = 0;
    bool m_requestPending = false;
    bool m_restartWhenFinished = false;
    QElapsedTimer m_failurePeriod;
    void failed(const QString &message);
};

} // namespace vusiif::zto::zerotier
