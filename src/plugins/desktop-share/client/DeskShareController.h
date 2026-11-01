#pragma once

#include "NetworkClient.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <functional>
#include <memory>
#include <ztmedia/MediaSession.h>
#include <ztmedia/MediaTypes.h>

namespace vusiif::zto::media {
class VideoPreview;
}

namespace vusiif::zto::desktop_share {

class Preferences;

// Desktop-share controller: the page's only source of truth.
//
// Two related but distinct observable states exist:
//
// * `state` keeps the established capture/transport contract used by the
//   stage-3/4/5 regressions (`idle`, `starting`, `live`, `waiting`,
//   `interrupted`, `stopping`, `stopped`, `failed`, `stop-failed`).
// * `phase` is the page lifecycle state machine required by stage 6. The QML
//   page binds exclusively to `phase`, `phaseLabel`, `statusText`,
//   `errorText`, `nextAction` and the `can*` capability flags; it never derives
//   a remote/room state of its own.
//
// MediaSession, track leases and every UI-facing operation stay on the host
// event (GUI) thread. Sockets, fragment splitting and reassembly live on the
// network thread inside NetworkClient; video crosses that boundary through
// bounded mailboxes only.
//
// Explicit-operation rules:
// * Opening the page, editing the address, selecting/refreshing a source never
//   connects or captures. `startSharing()` requires an established connection;
//   the legacy `createRoom()`/`joinRoom()` entries keep their stage-4
//   auto-connect behaviour for the archived regressions and are not reachable
//   from the page.
// * Every asynchronous result carries the connection/operation/room/media
//   generation it was started in. A cancelled, stopped or replaced operation
//   can therefore never restart capture, send, decode or overwrite newer state.
class DeskShareController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString phase READ phase NOTIFY changed)
    Q_PROPERTY(QString phaseLabel READ phaseLabel NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    Q_PROPERTY(QString nextAction READ nextAction NOTIFY changed)
    Q_PROPERTY(QString pendingOperation READ pendingOperation NOTIFY changed)
    Q_PROPERTY(QString previewHint READ previewHint NOTIFY changed)
    Q_PROPERTY(bool previewLive READ previewLive NOTIFY changed)
    Q_PROPERTY(bool canConnect READ canConnect NOTIFY changed)
    Q_PROPERTY(bool canDisconnect READ canDisconnect NOTIFY changed)
    Q_PROPERTY(bool canRefreshSources READ canRefreshSources NOTIFY changed)
    Q_PROPERTY(bool canSelectSource READ canSelectSource NOTIFY changed)
    Q_PROPERTY(bool canSelectVideo READ canSelectVideo NOTIFY changed)
    Q_PROPERTY(bool canStartShare READ canStartShare NOTIFY changed)
    Q_PROPERTY(bool canStopShare READ canStopShare NOTIFY changed)
    Q_PROPERTY(bool canJoin READ canJoin NOTIFY changed)
    Q_PROPERTY(bool canLeave READ canLeave NOTIFY changed)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY changed)
    Q_PROPERTY(bool canCopyRoom READ canCopyRoom NOTIFY changed)
    Q_PROPERTY(QString preferencesError READ preferencesError NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString networkState READ networkState NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString details READ details NOTIFY changed)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY changed)
    Q_PROPERTY(QString sourceValue READ sourceValue WRITE setSourceValue NOTIFY changed)
    Q_PROPERTY(QString profile READ profile WRITE setProfile NOTIFY changed)
    Q_PROPERTY(QString codec READ codec WRITE setCodec NOTIFY changed)
    Q_PROPERTY(bool capturing READ capturing NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(int decodedFrames READ decodedFrames NOTIFY changed)
    Q_PROPERTY(int capturedFrames READ capturedFrames NOTIFY changed)
    Q_PROPERTY(int frameWidth READ frameWidth NOTIFY changed)
    Q_PROPERTY(int frameHeight READ frameHeight NOTIFY changed)
    Q_PROPERTY(QString serverAddress READ serverAddress WRITE setServerAddress NOTIFY changed)
    Q_PROPERTY(QString roomCode READ roomCode NOTIFY changed)
    Q_PROPERTY(QString joinCode READ joinCode WRITE setJoinCode NOTIFY changed)
    Q_PROPERTY(int viewerCount READ viewerCount NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(bool publishing READ publishing NOTIFY changed)
    Q_PROPERTY(bool watching READ watching NOTIFY changed)

public:
    explicit DeskShareController(std::shared_ptr<media::MediaSession> session,
                                 QObject *parent = nullptr);
    ~DeskShareController() override;

    void setPreferences(Preferences *preferences);
    QString phase() const { return m_phase; }
    QString phaseLabel() const { return m_phaseLabel; }
    QString errorText() const { return m_errorText; }
    QString nextAction() const { return m_nextAction; }
    QString pendingOperation() const { return m_operation; }
    QString previewHint() const;
    bool previewLive() const;
    QString preferencesError() const { return m_preferencesError; }
    bool canConnect() const;
    bool canDisconnect() const;
    bool canRefreshSources() const;
    bool canSelectSource() const;
    bool canSelectVideo() const;
    bool canStartShare() const;
    bool canStopShare() const;
    bool canJoin() const;
    bool canLeave() const;
    bool canCancel() const;
    bool canCopyRoom() const;

    QString state() const { return m_state; }
    QString networkState() const { return m_networkState; }
    QString statusText() const { return m_status; }
    QString details() const { return m_details; }
    QVariantList sources() const { return m_sources; }
    QString sourceValue() const { return m_sourceValue; }
    void setSourceValue(const QString &value);
    QString profile() const { return m_profile; }
    void setProfile(const QString &profile);
    QString codec() const { return m_codec; }
    void setCodec(const QString &codec);
    bool capturing() const;
    bool busy() const;
    int decodedFrames() const { return m_decodedFrames; }
    int capturedFrames() const { return m_capturedFrames; }
    int frameWidth() const { return m_frameWidth; }
    int frameHeight() const { return m_frameHeight; }
    QString serverAddress() const { return m_serverAddress; }
    void setServerAddress(const QString &address);
    QString roomCode() const { return m_roomCode; }
    QString joinCode() const { return m_joinCode; }
    void setJoinCode(const QString &code);
    int viewerCount() const { return m_viewerCount; }
    bool connected() const { return m_connected; }
    bool publishing() const { return m_publisherRoom; }
    bool watching() const { return m_viewerRole && m_receiveReady; }

    Q_INVOKABLE void initialize();
    Q_INVOKABLE void refreshSources();
    // Legacy local loopback entry (stage-3 regression). The page never calls it.
    Q_INVOKABLE void startCapture();
    Q_INVOKABLE void stopCapture();
    // Page entry points. They never auto-connect.
    Q_INVOKABLE void startSharing();
    Q_INVOKABLE void stopSharing();
    Q_INVOKABLE void joinRoomExplicit();
    Q_INVOKABLE void leaveRoom();
    Q_INVOKABLE void cancelOperation();

    // Stage-4 entries kept for the archived regressions and the end-to-end
    // harness. createRoom()/joinRoom() keep the stage-4 behaviour of connecting
    // first when no connection exists; the page uses startSharing()/
    // joinRoomExplicit() instead.
    Q_INVOKABLE void connectServer();
    Q_INVOKABLE void disconnectServer();
    Q_INVOKABLE void createRoom();
    Q_INVOKABLE void joinRoom();

    // Observable snapshot without credentials or video content.
    Q_INVOKABLE QVariantMap statistics() const;
    // Lifecycle facts for evidence: phase, generations, queue and drain state.
    Q_INVOKABLE QVariantMap lifecycleStatistics() const;
    Q_INVOKABLE QVariantMap recoveryObservation() const;
    // Feedback cadence observation: request/response wall times, round trip,
    // acknowledgement-to-request gap and the server's rate-limit verdict. Used
    // by the end-to-end viewer so a normal static page can prove it never hit
    // RATE_LIMITED and so a genuine over-frequency attempt still shows it.
    Q_INVOKABLE QVariantMap feedbackObservation() const;

    // Called by the plugin before the media session is closed. Successful
    // completion means the network thread joined and every media track removal
    // completed; a false result retains the controller, DLL and lease so the
    // stop can be retried.
    bool shutdown(int timeoutMs = 5000);
    void shutdownAsync(std::function<void(bool)> completed, int timeoutMs = 5000);
    void setPreview(media::VideoPreview *preview);

signals:
    void changed();

private:
    struct Profile {
        QString id;
        int width;
        int height;
        int bitrate;
    };
    static Profile profileFor(const QString &id);
    media::TrackConfig videoConfig(bool send, const QString &source,
                                   const QByteArray &initializationData) const;
    media::TrackConfig receiveConfig(const StreamFormat &format) const;
    QString liveStatus() const;
    qint64 nowMs() const { return m_clock.elapsed(); }
    void tick();
    void packetArrived(media::EncodedPacket packet);
    void beginLoopback();
    void submitPacket(media::EncodedPacket packet);
    void frameArrived(const media::FrameView &frame);
    void mediaError(const media::MediaError &error);
    void startTracks();
    void fail(const QString &message);
    void failRoom(const QString &message, const QString &nextAction, const QString &reasonCode);
    void recordViewerFailure(const QString &reasonCode, const QString &message);
    void failConnectionAttempt(const QString &message);
    void clearTrackState();

    // Lifecycle state machine helpers.
    void setPhase(const QString &phase);
    void setOperation(const QString &operation);
    void clearOperation();
    void setError(const QString &message, const QString &nextAction);
    void clearError();
    bool isTransient() const;

    // Room/session helpers.
    void rebuildNetwork();
    void beginCapture();
    void restartPublishing();
    void requestLocalKeyFrame();
    void requestRemoteKeyFrame();
    void publishPacket(const media::EncodedPacket &packet);
    void beginViewing(const StreamFormat &format);
    void createViewerTrack(const StreamFormat &format);
    void teardownReceiveTrack();
    void teardownSendTrack();
    void endWatching(const QString &message, bool leaveRemote);
    void endPublishing(const QString &message, bool stopRemote);
    void updateNetworkStatus();
    void drainNetworkVideo();
    void onConnected(quint64 sessionId, quint16 udpPort);
    void onRoomCreated(quint64 roomId, const QString &code, const QString &profile,
                       const QByteArray &token, bool hasFormat, const StreamFormat &format);
    void onRoomJoined(quint64 roomId, const QString &code, const QString &profile,
                      const QByteArray &token, bool hasFormat, const StreamFormat &format);
    void onStreamConfig(const StreamFormat &format);
    void onControlFailed(const QString &operation, int code, const QString &message);

    // Preferences.
    void loadPreferences();
    void schedulePreferencesSave();
    void flushPreferences();
    void reportPreferencesError(const QString &message);
    bool hasDirtyPreferences() const { return m_dirtyAddress || m_dirtyProfile; }

    // ---- Media-track release bookkeeping (R1/R3) --------------------------
    //
    // A track identity is kept here from the moment its removal is requested
    // until the media provider confirms the release. The pending list is the
    // single authority for "the worker may still own this resource": a failed
    // or timed-out release keeps the identity so a retry targets the same
    // resource, and every conflicting room operation is refused while the list
    // is non-empty. Active ids are detached into this list first, so a new
    // initialisation can never overwrite an identity that is still pending.
    struct PendingRelease {
        QString id;
        bool send = true;
        bool inFlight = false;
        quint64 attempt = 0;
    };
    struct ReleaseDrain {
        std::function<void(bool)> done;
        bool finished = false;
    };
    bool hasPendingTracks() const { return !m_pendingReleases.isEmpty(); }
    void detachActiveTracks();
    void requestTrackRelease(const QString &id, bool send);
    void issueTrackRelease(const QString &id, quint64 attempt);
    void onTrackReleased(const QString &id, quint64 attempt, bool ok, const QString &error);
    void retryPendingReleases(std::function<void(bool)> completed);
    void completeReleaseDrain(bool ok);
    void recoverAfterLateRelease();
    QString pendingReleaseSummary() const;
    // R3 business-entry guard: refuses a conflicting operation while a track
    // release is unconfirmed and reports it visibly. Returns true when refused.
    bool refuseWhileReleasing();

    // ---- Stop task lifecycle (R2) -----------------------------------------
    //
    // Every stop/cleanup owns one task with its own generation. Only the
    // current task may write UI state, clear the active operation or complete
    // waiters; a task invalidated by a newer session or a connection loss can
    // only finish releasing the resources it detached. Repeated stop/unload
    // requests merge into the running task instead of starting a second drain.
    struct StopTask {
        quint64 generation = 0;
        bool unload = false;
        bool leave = false;
        bool hadRoom = false;
        bool mediaDrained = false;
        bool networkDrained = false;
        QString status;
        QString error;
        QList<std::function<void(bool)>> waiters;
    };
    std::shared_ptr<StopTask> beginStopTask(bool leave, bool unload, const QString &status,
                                            const QString &error,
                                            std::function<void(bool)> completed);
    void upgradeStopToUnload(const std::shared_ptr<StopTask> &task, int timeoutMs);
    bool isStopTaskCurrent(const std::shared_ptr<StopTask> &task) const;
    void startStopMediaDrain(const std::shared_ptr<StopTask> &task);
    void startStopNetworkDrain(const std::shared_ptr<StopTask> &task);
    void finishStopTask(const std::shared_ptr<StopTask> &task, bool ok);
    void abandonStopTask(const QString &reason);
    void beginRoomStop(bool leave, const QString &status, const QString &error,
                       std::function<void(bool)> completed);
    void drainTracks(std::function<void(bool)> completed);
    void clearRoomIdentity();

    std::shared_ptr<media::MediaSession> m_session;
    Preferences *m_preferences = nullptr;
    std::unique_ptr<NetworkClient> m_network;
    QPointer<media::VideoPreview> m_preview;
    QTimer m_watchdog;
    QTimer m_preferencesSaveTimer;
    qint64 m_lastMediaStatsMs = 0;
    QVariantMap m_mediaSendStats;
    QVariantMap m_mediaReceiveStats;
    QElapsedTimer m_clock;
    QString m_phase = QStringLiteral("disconnected");
    QString m_phaseLabel;
    QString m_operation;
    QString m_errorText;
    QString m_nextAction;
    QString m_preferencesError;
    QString m_state = QStringLiteral("idle");
    QString m_networkState = QStringLiteral("disconnected");
    QString m_status = QStringLiteral("打开本页不会联网或采集；请填写服务器地址。");
    QString m_details;
    QVariantList m_sources;
    QString m_sourceValue, m_profile = QStringLiteral("720p30");
    // Frame rate is the priority for 8-viewer rooms on constrained links: the
    // H.264 profile keeps a 30 fps stream near the 2 Mbps budget, while the
    // lossless tile mode can exceed 1.3 Mbps per viewer for moving content.
    QString m_codec = QStringLiteral("h264");
    QString m_sendId, m_receiveId;
    media::TrackDescriptor m_receiveDescriptor;
    QByteArray m_initializationData;
    bool m_receiveReady = false;
    bool m_receiveStarting = false;
    bool m_sessionOpen = false;
    bool m_opening = false;
    bool m_shuttingDown = false;
    bool m_acceptingFrames = false;
    bool m_sourceMissing = false;
    bool m_preferencesLoaded = false;
    // Field-level dirtiness: only the fields this controller actually edited are
    // merged into the newest preferences file (R4).
    bool m_dirtyAddress = false;
    bool m_dirtyProfile = false;
    bool m_unloading = false;
    quint64 m_epoch = 0;
    quint64 m_sourceEpoch = 0;
    quint64 m_lifecycle = 0;
    quint64 m_observationGeneration = 0;
    QVariantMap m_observedStream, m_viewerFailure;
    int m_capturedFrames = 0, m_decodedFrames = 0;
    int m_frameWidth = 0, m_frameHeight = 0;
    quint64 m_dropped = 0;
    qint64 m_lastPacketMs = 0, m_lastDecodedMs = 0, m_lastKeyRequestMs = 0;
    qint64 m_lastStatisticsMs = 0;
    qint64 m_startRequestedMs = 0;
    qint64 m_sourceMissingSinceMs = 0;

    // Network identity and stage-4 stream state.
    QString m_serverAddress;
    quint16 m_serverPort = DefaultPort;
    bool m_connected = false;
    quint64 m_sessionId = 0;
    quint16 m_udpPort = 0;
    quint64 m_roomId = 0;
    QString m_roomCode;
    QString m_joinCode;
    QByteArray m_roomToken;
    int m_viewerCount = 0;
    bool m_publisherRoom = false;
    bool m_viewerRole = false;
    bool m_pendingCreate = false;
    bool m_pendingJoin = false;
    quint64 m_publishEpoch = 0;
    quint64 m_nextWireSequence = 0;
    bool m_configurePending = false;
    bool m_awaitingKeyFrame = false;
    quint64 m_viewEpoch = 0;
    StreamFormat m_viewFormat;
    qint64 m_viewStartMs = 0;
    qint64 m_lastNetworkPacketMs = 0;
    bool m_viewFailed = false;
    bool m_gapAfterDecodedFrame = false;
    quint64 m_networkSubmitted = 0;
    quint64 m_uploadDropped = 0;
    quint64 m_gapDetected = 0;
    int m_submitFailures = 0;
    // Feedback pacing is measured from the previous server acknowledgement, not
    // from the previous send. The server rejects any pair of reports that reach
    // it inside one second; a client that schedules from its own send time has
    // no margin for the network trip and can be compressed below the window.
    bool m_feedbackPending = false;
    qint64 m_feedbackSentMs = 0;
    qint64 m_feedbackAckMs = 0;
    qint64 m_feedbackRequestEpochMs = 0;
    qint64 m_feedbackResponseEpochMs = 0;
    qint64 m_feedbackLastRoundTripMs = 0;
    qint64 m_feedbackLastAckToRequestMs = 0;
    qint64 m_feedbackMinAckToRequestMs = 0;
    quint64 m_feedbackRequests = 0;
    quint64 m_feedbackResponses = 0;
    quint64 m_feedbackRateLimited = 0;
    quint64 m_feedbackAckTimeouts = 0;
    qint64 m_lastRemoteKeyRequestMs = 0;
    QString m_lastNetworkError;

    // Active stop/drain operation.
    bool m_stopping = false;
    bool m_networkDrained = false;
    bool m_mediaDrained = false;
    bool m_tracksPending = false;
    QList<PendingRelease> m_pendingReleases;
    quint64 m_releaseAttempt = 0;
    std::shared_ptr<ReleaseDrain> m_releaseDrain;
    quint64 m_releaseFailures = 0;
    QString m_lastReleaseError;
    std::shared_ptr<StopTask> m_stopTask;
    quint64 m_stopGeneration = 0;
    int m_stopTimeoutMs = 5000;
    bool m_stopUnload = false;
};

} // namespace vusiif::zto::desktop_share
