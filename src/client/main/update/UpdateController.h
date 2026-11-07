#pragma once

#include "UpdateArchive.h"
#include "UpdateHttp.h"
#include "UpdateManifest.h"
#include "UpdateStorage.h"
#include "UpdateVersion.h"

#include <QObject>
#include <QString>

#include <functional>

class QTimer;
class QProcess;

namespace vusiif::zto::update {

// Automatic update coordinator.
//
// Public QML contract (also documented in docs/auto-update.md):
//   state            QString  idle|checking|downloading|verifying|staging|preparing|uptodate|failed|unconfigured
//   status           QString  human-readable Chinese status line
//   busy             bool     true while a check/download/stage runs
//   progress         int      0..100 download progress (100 while verifying)
//   availableVersion QString  version offered by the manifest ("" when none)
//   lastError        QString  last failure reason ("" when the last run was fine)
//   feedUrl          QString  user-configured manifest URL (empty disables checks)
//   configured       bool     both a manifest URL and a trusted key are present
//   currentVersion   QString  QCoreApplication::applicationVersion() (VERSION)
//   installedMessage QString  result of the previous helper transaction
//   updateAvailable  bool     a strictly newer artifact is ready to install
//
// Signals:
//   notificationRequested(const QString &message, bool error)
//   quitRequested()   // host must call AppController::requestQuit()
//
// Methods:
//   checkManually(), checkOnStartup(), setServer(), retry(), busy()
class UpdateController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(int progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY stateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)
    Q_PROPERTY(QString feedUrl READ feedUrl NOTIFY stateChanged)
    Q_PROPERTY(QString serverIp READ serverIp NOTIFY stateChanged)
    Q_PROPERTY(int serverPort READ serverPort NOTIFY stateChanged)
    Q_PROPERTY(bool configured READ configured NOTIFY stateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString installedMessage READ installedMessage NOTIFY stateChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY stateChanged)
public:
    explicit UpdateController(const QString &executableDirectory, const QString &storageRoot,
                              const QString &settingsFile, QObject *parent = nullptr);
    ~UpdateController() override;

    // Other host operations (service restart, plugin install, ...) must not run
    // while an update is being staged. The provider is polled at check time.
    void setHostBusyProvider(std::function<bool()> provider);

    Q_INVOKABLE void checkManually();
    Q_INVOKABLE void checkOnStartup();
    Q_INVOKABLE void retry();
    bool busy() const;

    // Persists IP/port as update/server (empty IP disables checks).
    // The trusted public key can never be changed at runtime.
    Q_INVOKABLE bool setServer(const QString &ip, int port = 18102);
    QString serverIp() const;
    int serverPort() const;

    QString state() const { return m_state; }
    QString status() const { return m_status; }
    int progress() const { return m_progress; }
    QString availableVersion() const { return m_availableVersion; }
    QString lastError() const { return m_lastError; }
    QString feedUrl() const { return m_feedUrl; }
    bool configured() const { return m_configured; }
    QString currentVersion() const { return m_currentVersion; }
    QString installedMessage() const { return m_installedMessage; }
    bool updateAvailable() const { return m_updateAvailable; }

    // Test hooks: the isolated tests replace the helper with a stub and disable
    // the restart. Production code must not call these.
    void setHelperExecutableForTesting(const QString &path) { m_helperOverride = path; }
    void setNoRestartForTesting(bool enabled) { m_noRestartForTesting = enabled; }

signals:
    void stateChanged();
    void notificationRequested(const QString &message, bool error);
    void quitRequested();

private:
    enum class Phase { Idle, Checking, Downloading, Verifying, Staging, Preparing };

    void startCheck(bool manual);
    void handleManifest(bool ok, const QByteArray &bytes, const UpdateManifest &manifest,
                        const UpdateArtifact &artifact, const QString &error);
    void startDownload();
    void handleDownload(bool ok, const QString &path, const QString &error);
    void stagePackage();
    void handleStaged();
    void prepareAndQuit();
    void fail(const QString &error, bool notify);
    void notify(const QString &message, bool error);
    void refreshConfiguration();
    void processStartupResult();
    void setPhase(Phase phase);
    void setState(const QString &state, const QString &status);
    bool readyForCheck(bool manual);
    void scheduleStartupRetry();
    void disposeHttp();

    QString m_executableDirectory;
    QString m_storageRoot;
    QString m_settingsFile;
    QString m_currentVersion;
    QString m_feedUrl;
    QString m_serverAddress;
    QString m_state = QStringLiteral("idle");
    QString m_status;
    QString m_lastError;
    QString m_installedMessage;
    QString m_availableVersion;
    int m_progress = 0;
    bool m_configured = false;
    bool m_updateAvailable = false;
    bool m_manual = false;
    bool m_startupHandled = false;
    bool m_startupMaintenanceDone = false;
    bool m_pendingResultNotify = false;
    bool m_pendingResultError = false;
    bool m_manualRetryPending = false;
    bool m_noRestartForTesting = false;
    QString m_helperOverride;
    int m_startupAttempts = 0;

    Phase m_phase = Phase::Idle;
    UpdateHttp *m_http = nullptr;
    QTimer *m_startupTimer = nullptr;
    QProcess *m_downloader = nullptr;
    std::function<bool()> m_hostBusyProvider;

    UpdatePaths m_paths;
    UpdateManifest m_manifest;
    UpdateArtifact m_artifact;
    QString m_downloadPath;
    QString m_stagingDirectory;
    QList<ExtractedFile> m_extractedFiles;
    QByteArray m_publicKey;
};

} // namespace vusiif::zto::update
