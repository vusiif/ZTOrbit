#pragma once
#include "SystemOperations.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

class QTemporaryDir;

namespace vusiif::zto::reset {

// Owns the confirmation, mutual exclusion, one elevated operation and the
// user-facing result for "reset local ZeroTier state". It never talks to the
// ZeroTier API directly: the elevated script stops the service, deletes only
// whitelisted state files, merges local.conf, restarts the service and runs
// the existing local-API readiness probe.
class ResetController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool canReset READ canReset NOTIFY canResetChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY statusChanged)
public:
    explicit ResetController(ui::SystemOperations &operations, QObject *parent = nullptr);
    ~ResetController() override;

    // True while another host operation runs. Injected by the host.
    void setBlockedProvider(std::function<bool()> blocked);

    Q_INVOKABLE void requestReset();
    Q_INVOKABLE void confirm(bool accepted);

    bool busy() const { return m_busy; }
    bool canReset() const;
    QString status() const { return m_status; }
    QString lastError() const { return m_lastError; }

signals:
    void notificationRequested(const QString &message, bool error);
    void confirmationRequested(const QString &title, const QString &message, const QString &acceptLabel);
    void interruptionBegan();
    void interruptionEnded();
    void busyChanged();
    void canResetChanged();
    void statusChanged();

private:
    void beginReset();
    void finishElevation(bool ok);
    void failBeforeElevation(const QString &message);
    void completeRun(const QString &message, bool error);
    void endBusy();
    void setStatus(const QString &status);
    void notify(const QString &message, bool error);
    bool readLocalConfigBytes(QByteArray &bytes, bool &exists, QString &error);
    QString outcomeMessage(bool ok) const;

    ui::SystemOperations &m_operations;
    std::function<bool()> m_blocked;
    std::shared_ptr<QTemporaryDir> m_workDirectory;
    QMetaObject::Connection m_outputConnection;
    QStringList m_scriptOutput;
    QString m_status;
    QString m_lastError;
    int m_removalCount = 0;
    bool m_localConfigChanged = false;
    bool m_busy = false;
    bool m_awaitingConfirmation = false;
    bool m_interruptionActive = false;
    // Parsed markers from the elevated script.
    bool m_sawElevated = false;
    bool m_sawDone = false;
    bool m_failed = false;
    bool m_recoveryFailed = false;
    bool m_notInstalled = false;
    QString m_failedStage;
    QString m_failureDetail;
};

} // namespace vusiif::zto::reset
