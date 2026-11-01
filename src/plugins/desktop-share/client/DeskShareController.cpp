#include "DeskShareController.h"

#include "Preferences.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonDocument>
#include <QThread>
#include <QTimer>
#include <QVariantMap>
#include <QtEndian>

#include <ztmedia/VideoPreview.h>

namespace vusiif::zto::desktop_share {

namespace {
constexpr int FrameRate = 30;
constexpr int KeyFrameInterval = 30;
constexpr qint64 FirstFrameTimeoutMs = 10000;
constexpr qint64 WaitingAfterMs = 3000;
constexpr qint64 InterruptedAfterMs = 10000;
constexpr qint64 SourceMissingTimeoutMs = 3000;
constexpr int StopTrackDrainMs = 3000;
constexpr int StopGraceMs = 50;
// The server accepts at most one viewer feedback report per second, measured
// between its own processing times. Scheduling the next report one second after
// the previous send leaves no margin for the network trip, so a normal one-per-
// second viewer could be compressed below the window and receive RATE_LIMITED.
// Pace from the acknowledgement instead: the round trip then sits between the
// two server processing instants and the window can never be violated.
constexpr qint64 FeedbackMinIntervalMs = 1000;
// A control request expires after five seconds. If no feedback acknowledgement
// arrives with margin after that, re-arm rather than stall reporting forever.
constexpr qint64 FeedbackAckTimeoutMs = 6000;

QString phaseText(const QString &phase)
{
    if (phase == QStringLiteral("disconnected")) return QStringLiteral("未连接");
    if (phase == QStringLiteral("connecting")) return QStringLiteral("连接中");
    if (phase == QStringLiteral("connected")) return QStringLiteral("已连接");
    if (phase == QStringLiteral("ready")) return QStringLiteral("准备共享");
    if (phase == QStringLiteral("creating")) return QStringLiteral("正在创建房间");
    if (phase == QStringLiteral("joining")) return QStringLiteral("正在加入房间");
    if (phase == QStringLiteral("startingShare")) return QStringLiteral("正在启动共享");
    if (phase == QStringLiteral("waitingSource")) return QStringLiteral("等待来源");
    if (phase == QStringLiteral("waitingFirstFrame")) return QStringLiteral("等待首帧");
    if (phase == QStringLiteral("sharing")) return QStringLiteral("正在共享");
    if (phase == QStringLiteral("viewerWaiting")) return QStringLiteral("等待共享者");
    if (phase == QStringLiteral("viewing")) return QStringLiteral("正在观看");
    if (phase == QStringLiteral("waitingNewFrame")) return QStringLiteral("等待新画面");
    if (phase == QStringLiteral("mediaInterrupted")) return QStringLiteral("媒体中断");
    if (phase == QStringLiteral("stopping")) return QStringLiteral("停止中");
    if (phase == QStringLiteral("ended")) return QStringLiteral("已结束");
    if (phase == QStringLiteral("failed")) return QStringLiteral("失败");
    if (phase == QStringLiteral("stopFailed")) return QStringLiteral("停止失败");
    if (phase == QStringLiteral("previewing")) return QStringLiteral("本地预览");
    return phase;
}
} // namespace

DeskShareController::DeskShareController(std::shared_ptr<media::MediaSession> session,
                                         QObject *parent)
    : QObject(parent), m_session(std::move(session)), m_network(std::make_unique<NetworkClient>(this))
{
    m_clock.start();
    m_phaseLabel = phaseText(m_phase);
    if (m_session) {
        m_session->packet = [this](media::EncodedPacket packet) { packetArrived(std::move(packet)); };
        m_session->frame = [this](const media::FrameView &frame) { frameArrived(frame); };
        m_session->error = [this](const media::MediaError &error) { mediaError(error); };
        m_session->keyFrameRequired = [this](const QString &track) {
            if (m_shuttingDown || m_stopping || !m_acceptingFrames || track != m_receiveId) return;
            if (m_viewerRole) {
                requestRemoteKeyFrame();
                return;
            }
            requestLocalKeyFrame();
        };
        m_session->sourcesChanged = [this] { refreshSources(); };
    }
    m_network->start();
    connect(m_network.get(), &NetworkClient::connected, this,
            [this](quint64 session, quint16 port) { onConnected(session, port); });
    connect(m_network.get(), &NetworkClient::connectFailed, this, [this](const QString &message) {
        if (m_shuttingDown) return;
        failConnectionAttempt(QStringLiteral("连接服务端失败：%1").arg(message));
    });
    connect(m_network.get(), &NetworkClient::connectionLost, this, [this](const QString &message) {
        if (m_shuttingDown) return;
        // R2: a connection loss abandons the running stop task. Its completion
        // callback becomes stale immediately and may only finish releasing the
        // tracks it detached; it must never write state, clear a newer
        // operation or complete a newer session's waiters.
        abandonStopTask(QStringLiteral("控制连接已断开"));
        recordViewerFailure(QStringLiteral("CONTROL_CONNECTION_LOST"), message);
        const bool hadRoom = m_publisherRoom || m_viewerRole;
        m_connected = false;
        m_sessionId = 0;
        m_udpPort = 0;
        m_networkState = QStringLiteral("failed");
        m_pendingCreate = false;
        m_pendingJoin = false;
        ++m_epoch;
        ++m_viewEpoch;
        ++m_lifecycle;
        m_acceptingFrames = false;
        clearRoomIdentity();
        m_publisherRoom = false;
        m_viewerRole = false;
        m_configurePending = false;
        m_awaitingKeyFrame = false;
        m_state = QStringLiteral("failed");
        m_lastNetworkError = message;
        setError(QStringLiteral("控制连接已断开：%1").arg(message),
                 QStringLiteral("检查网络与服务端后重新连接；服务端已按断线规则关闭房间。"));
        m_status = m_errorText;
        m_operation.clear();
        m_stopping = false;
        detachActiveTracks();
        if (m_preview) m_preview->clear();
        setPhase(QStringLiteral("disconnected"));
        if (hadRoom) m_details = QStringLiteral("连接已断开，房间与媒体资源已回收。");
        m_watchdog.start();
        emit changed();
    });
    connect(m_network.get(), &NetworkClient::roomCreated, this,
            [this](quint64 room, const QString &code, const QString &profile, const QByteArray &token,
                   bool hasFormat, const StreamFormat &format) {
                onRoomCreated(room, code, profile, token, hasFormat, format);
            });
    connect(m_network.get(), &NetworkClient::roomJoined, this,
            [this](quint64 room, const QString &code, const QString &profile, const QByteArray &token,
                   bool hasFormat, const StreamFormat &format) {
                onRoomJoined(room, code, profile, token, hasFormat, format);
            });
    connect(m_network.get(), &NetworkClient::roomStateChanged, this,
            [this](quint64 room, int, int viewers) {
                if (m_shuttingDown || room != m_roomId || m_stopping) return;
                m_viewerCount = viewers;
                emit changed();
            });
    connect(m_network.get(), &NetworkClient::streamConfigReceived, this,
            [this](quint64 room, const StreamFormat &format) {
                if (m_shuttingDown || m_stopping || room != m_roomId) return;
                onStreamConfig(format);
            });
    connect(m_network.get(), &NetworkClient::streamKeyframeRequested, this,
            [this](quint64 room, quint64 epoch) {
                if (m_shuttingDown || m_stopping || room != m_roomId || !m_publisherRoom) return;
                if (epoch != m_publishEpoch) return;
                requestLocalKeyFrame();
            });
    connect(m_network.get(), &NetworkClient::streamConfigured, this, [this](quint64 epoch) {
        if (m_shuttingDown || m_stopping || !m_publisherRoom) return;
        m_publishEpoch = epoch;
        m_configurePending = false;
        m_nextWireSequence = 0;
        m_awaitingKeyFrame = true;
        m_status = QStringLiteral("服务端已分配视频流代次 %1，正在等待新的关键帧…").arg(epoch);
        requestLocalKeyFrame();
        emit changed();
    });
    connect(m_network.get(), &NetworkClient::streamReadyConfirmed, this, [this](quint64 epoch) {
        if (m_shuttingDown || m_stopping || !m_viewerRole || epoch != m_viewEpoch) return;
        m_status = QStringLiteral("接收端已确认代次 %1，等待首个关键帧…").arg(epoch);
        emit changed();
    });
    connect(m_network.get(), &NetworkClient::feedbackCompleted, this, [this](bool ok, int code) {
        if (m_shuttingDown) return;
        const auto acknowledged = nowMs();
        m_feedbackPending = false;
        m_feedbackAckMs = acknowledged;
        m_feedbackResponseEpochMs = QDateTime::currentMSecsSinceEpoch();
        ++m_feedbackResponses;
        if (m_feedbackSentMs > 0) m_feedbackLastRoundTripMs = acknowledged - m_feedbackSentMs;
        if (!ok) {
            if (ErrorCode(code) == ErrorCode::RateLimited) ++m_feedbackRateLimited;
            return;
        }
    });
    connect(m_network.get(), &NetworkClient::roomClosed, this, [this](quint64 room, int reason) {
        if (m_shuttingDown || m_stopping || room != m_roomId) return;
        const auto name = closeReasonName(CloseReason(reason));
        const bool wasPublisher = m_publisherRoom;
        const bool wasViewer = m_viewerRole;
        recordViewerFailure(QStringLiteral("ROOM_CLOSED"), name);
        ++m_epoch;
        ++m_viewEpoch;
        ++m_lifecycle;
        m_acceptingFrames = false;
        clearRoomIdentity();
        m_publisherRoom = false;
        m_viewerRole = false;
        m_configurePending = false;
        m_awaitingKeyFrame = false;
        detachActiveTracks();
        if (m_preview) m_preview->clear();
        if (wasPublisher) {
            m_state = QStringLiteral("failed");
            setError(QStringLiteral("共享已结束：%1").arg(name),
                     QStringLiteral("重新选择来源后再次点击“开始共享”。"));
        } else if (wasViewer) {
            m_state = QStringLiteral("failed");
            setError(QStringLiteral("观看已结束：%1").arg(name),
                     QStringLiteral("核对房号后重新点击“加入观看”。"));
        } else {
            setError(QStringLiteral("房间已关闭：%1").arg(name), QString());
        }
        m_status = m_errorText;
        m_networkState = m_connected ? QStringLiteral("connected") : QStringLiteral("disconnected");
        setPhase(m_connected ? QStringLiteral("failed") : QStringLiteral("disconnected"));
        emit changed();
    });
    connect(m_network.get(), &NetworkClient::udpReady, this, [this](bool ok, const QString &message) {
        if (m_shuttingDown || m_stopping || ok) return;
        m_lastNetworkError = message;
        if (m_publisherRoom) {
            failRoom(QStringLiteral("媒体端点不可用：%1，已停止共享并关闭房间").arg(message),
                     QStringLiteral("检查本机 UDP 与零信任网络后重新开始共享。"), QStringLiteral("UDP_ENDPOINT_UNAVAILABLE"));
        } else if (m_viewerRole) {
            failRoom(QStringLiteral("媒体端点不可用：%1").arg(message),
                     QStringLiteral("检查本机 UDP 与零信任网络后重新加入观看。"), QStringLiteral("UDP_ENDPOINT_UNAVAILABLE"));
        }
    });
    connect(m_network.get(), &NetworkClient::streamGapDetected, this, [this] {
        if (m_shuttingDown || m_stopping || !m_viewerRole) return;
        ++m_gapDetected;
        m_gapAfterDecodedFrame = true;
        m_status = QStringLiteral("检测到编码包缺失，等待新关键帧…");
        if (m_phase == QStringLiteral("viewing")) setPhase(QStringLiteral("waitingNewFrame"));
        requestRemoteKeyFrame();
        emit changed();
    });
    connect(m_network.get(), &NetworkClient::uploadDropped, this,
            [this](int packets, const QString &reason) {
                if (m_shuttingDown || m_stopping || !m_publisherRoom) return;
                m_uploadDropped += quint64(packets);
                m_awaitingKeyFrame = true;
                m_status = QStringLiteral("上传队列丢弃 %1 个编码包（%2），正在请求关键帧…")
                               .arg(packets)
                               .arg(reason);
                requestLocalKeyFrame();
                emit changed();
            });
    connect(m_network.get(), &NetworkClient::controlFailed, this,
            [this](const QString &operation, int code, const QString &message) {
                onControlFailed(operation, code, message);
            });
    connect(m_network.get(), &NetworkClient::incomingVideo, this,
            [this] { drainNetworkVideo(); });
    m_watchdog.setInterval(500);
    connect(&m_watchdog, &QTimer::timeout, this, [this] { tick(); });
    m_watchdog.start();
    m_preferencesSaveTimer.setSingleShot(true);
    m_preferencesSaveTimer.setInterval(400);
    connect(&m_preferencesSaveTimer, &QTimer::timeout, this, [this] { flushPreferences(); });
}

DeskShareController::~DeskShareController()
{
    m_watchdog.stop();
    m_preferencesSaveTimer.stop();
    flushPreferences();
    ++m_epoch;
    ++m_viewEpoch;
    m_acceptingFrames = false;
    if (m_network) m_network->shutdown();
    if (!m_session) return;
    m_session->packet = {};
    m_session->frame = {};
    m_session->error = {};
    m_session->keyFrameRequired = {};
    m_session->sourcesChanged = {};
    const auto send = m_sendId, receive = m_receiveId;
    clearTrackState();
    if (!send.isEmpty()) m_session->remove(send, this);
    if (!receive.isEmpty()) m_session->remove(receive, this);
    // Identities whose release was never confirmed are retried once more; the
    // lease close that follows also releases whatever the worker still holds.
    for (const auto &pending : m_pendingReleases) m_session->remove(pending.id, this);
    m_pendingReleases.clear();
    m_tracksPending = false;
}

void DeskShareController::setPreferences(Preferences *preferences)
{
    m_preferences = preferences;
}

DeskShareController::Profile DeskShareController::profileFor(const QString &id)
{
    if (id == QStringLiteral("1080p30")) return {id, 1920, 1080, 4000000};
    return {QStringLiteral("720p30"), 1280, 720, 2000000};
}

media::TrackConfig DeskShareController::videoConfig(bool send, const QString &source,
                                                    const QByteArray &initializationData) const
{
    const auto profile = profileFor(m_profile);
    media::TrackConfig config;
    config.kind = QStringLiteral("video");
    config.direction = send ? QStringLiteral("send") : QStringLiteral("receive");
    config.source = send ? source : QString();
    config.video.width = profile.width;
    config.video.height = profile.height;
    config.video.pixelFormat = QStringLiteral("bgra");
    config.video.colorSpace = QStringLiteral("bt709");
    config.video.colorRange = m_codec == QStringLiteral("bgra-tiles") ? QStringLiteral("full")
                                                                     : QStringLiteral("limited");
    config.codec.codec = m_codec;
    config.codec.bitrate = profile.bitrate;
    config.codec.frameRate = FrameRate;
    config.codec.keyFrameInterval = KeyFrameInterval;
    config.codec.allowAdjustment = false;
    config.codec.preferHardware = false;
    config.codec.initializationData = initializationData;
    config.preview = !send;
    return config;
}

media::TrackConfig DeskShareController::receiveConfig(const StreamFormat &format) const
{
    media::TrackConfig config;
    config.kind = QStringLiteral("video");
    config.direction = QStringLiteral("receive");
    config.video.width = format.width;
    config.video.height = format.height;
    config.video.pixelFormat = QStringLiteral("bgra");
    config.video.colorSpace = QStringLiteral("bt709");
    config.video.colorRange = format.codec == QStringLiteral("bgra-tiles")
                                  ? QStringLiteral("full")
                                  : QStringLiteral("limited");
    config.codec.codec = format.codec;
    config.codec.bitrate = format.bitrate;
    config.codec.frameRate = format.fps;
    config.codec.keyFrameInterval = KeyFrameInterval;
    config.codec.allowAdjustment = false;
    config.codec.preferHardware = false;
    config.codec.initializationData = format.initializationData;
    config.preview = true;
    config.recordAllowed = false;
    return config;
}

QString DeskShareController::liveStatus() const
{
    const auto profile = profileFor(m_profile);
    if (m_publisherRoom) {
        if (m_codec == QStringLiteral("bgra-tiles"))
            return QStringLiteral("正在共享：%1×%2 · 变化块优先 · %3 Mbps 媒体预算 · 观看者 %4")
                .arg(profile.width).arg(profile.height)
                .arg(profile.bitrate / 1000000.0, 0, 'f', 1).arg(m_viewerCount);
        return QStringLiteral("正在共享：%1×%2 · 30 fps 目标 · %3 Mbps · 观看者 %4")
            .arg(profile.width).arg(profile.height)
            .arg(profile.bitrate / 1000000.0, 0, 'f', 1).arg(m_viewerCount);
    }
    if (m_codec == QStringLiteral("bgra-tiles"))
        return QStringLiteral("正在本地回送预览：%1×%2 · 变化块优先 · %3 Mbps 媒体预算")
            .arg(profile.width).arg(profile.height).arg(profile.bitrate/1000000.0,0,'f',1);
    return QStringLiteral("正在本地回送预览：%1×%2 · 30 fps 目标 · %3 Mbps · H.264 软件编码")
        .arg(profile.width)
        .arg(profile.height)
        .arg(profile.bitrate / 1000000.0, 0, 'f', 1);
}

bool DeskShareController::capturing() const
{
    return m_state == QStringLiteral("starting") || m_state == QStringLiteral("live")
        || m_state == QStringLiteral("waiting") || m_state == QStringLiteral("interrupted");
}

bool DeskShareController::busy() const
{
    return !m_operation.isEmpty();
}

void DeskShareController::setPhase(const QString &phase)
{
    if (!m_viewerFailure.isEmpty() && !m_viewerRole && !m_stopping
        && (phase == QStringLiteral("failed") || phase == QStringLiteral("disconnected"))
        && !m_viewerFailure.contains(QStringLiteral("visibleEpochMs")))
        m_viewerFailure.insert(QStringLiteral("visibleEpochMs"), double(QDateTime::currentMSecsSinceEpoch()));
    if (m_phase == phase) return;
    m_phase = phase;
    m_phaseLabel = phaseText(phase);
}

void DeskShareController::setOperation(const QString &operation)
{
    if (operation == QStringLiteral("connect") || operation == QStringLiteral("join")) {
        m_observedStream.clear();
        m_viewerFailure.clear();
    }
    m_operation = operation;
}

void DeskShareController::clearOperation()
{
    m_operation.clear();
}

void DeskShareController::setError(const QString &message, const QString &nextAction)
{
    m_errorText = message;
    m_nextAction = nextAction;
}

void DeskShareController::clearError()
{
    m_errorText.clear();
    m_nextAction.clear();
}

bool DeskShareController::isTransient() const
{
    return m_operation == QStringLiteral("connect") || m_operation == QStringLiteral("create")
        || m_operation == QStringLiteral("join") || m_operation == QStringLiteral("start")
        || m_operation == QStringLiteral("stop") || m_operation == QStringLiteral("leave")
        || m_operation == QStringLiteral("disconnect");
}

QString DeskShareController::previewHint() const
{
    if (m_phase == QStringLiteral("waitingSource")) return QStringLiteral("等待来源首帧…");
    if (m_phase == QStringLiteral("waitingFirstFrame")) return QStringLiteral("等待首个解码画面…");
    if (m_phase == QStringLiteral("viewerWaiting")) return QStringLiteral("已加入房间，等待共享者开始共享…");
    if (m_phase == QStringLiteral("waitingNewFrame") || m_phase == QStringLiteral("mediaInterrupted"))
        return QStringLiteral("等待新画面…");
    if (m_phase == QStringLiteral("sharing") || m_phase == QStringLiteral("viewing")
        || m_phase == QStringLiteral("previewing"))
        return QStringLiteral("实时画面");
    if (m_phase == QStringLiteral("stopping")) return QStringLiteral("正在停止并释放资源…");
    if (m_phase == QStringLiteral("ended")) return QStringLiteral("共享已结束");
    return QStringLiteral("共享预览");
}

bool DeskShareController::previewLive() const
{
    return m_acceptingFrames && !m_stopping && m_decodedFrames > 0 && !m_receiveId.isEmpty();
}

bool DeskShareController::canConnect() const
{
    return !m_shuttingDown && !busy() && !m_connected;
}

bool DeskShareController::canDisconnect() const
{
    return !m_shuttingDown && m_connected && !busy();
}

bool DeskShareController::canRefreshSources() const
{
    return !m_shuttingDown && !busy() && !m_publisherRoom && !m_viewerRole && !capturing();
}

bool DeskShareController::canSelectSource() const
{
    // R3: a source change is a reconfiguration, so it is refused while the
    // worker has not confirmed the release of an older track.
    return !m_shuttingDown && !busy() && !hasPendingTracks() && !m_viewerRole
        && (!capturing() || m_publisherRoom);
}

bool DeskShareController::canSelectVideo() const
{
    return !m_shuttingDown && !busy() && !hasPendingTracks() && (!capturing() || m_publisherRoom);
}

bool DeskShareController::canStartShare() const
{
    return !m_shuttingDown && m_connected && !busy() && !hasPendingTracks() && !m_publisherRoom
        && !m_viewerRole && !capturing() && !m_sourceValue.isEmpty() && !m_sourceMissing;
}

bool DeskShareController::canStopShare() const
{
    return !m_shuttingDown && !busy()
        && (m_publisherRoom || m_viewerRole || capturing() || hasPendingTracks()
            || m_tracksPending);
}

bool DeskShareController::canJoin() const
{
    return !m_shuttingDown && m_connected && !busy() && !hasPendingTracks() && !m_publisherRoom
        && !m_viewerRole && !capturing();
}

bool DeskShareController::canLeave() const
{
    return !m_shuttingDown && m_viewerRole && !busy();
}

bool DeskShareController::canCancel() const
{
    return !m_shuttingDown && (m_operation == QStringLiteral("connect")
                               || m_operation == QStringLiteral("create")
                               || m_operation == QStringLiteral("join")
                               || m_operation == QStringLiteral("start"));
}

bool DeskShareController::canCopyRoom() const
{
    return !m_shuttingDown && m_publisherRoom && !m_roomCode.isEmpty();
}

void DeskShareController::setServerAddress(const QString &address)
{
    if (m_shuttingDown) return;
    const auto trimmed = address.trimmed();
    if (trimmed == m_serverAddress) return;
    m_serverAddress = trimmed;
    m_dirtyAddress = true;
    schedulePreferencesSave();
    emit changed();
}

void DeskShareController::setJoinCode(const QString &code)
{
    if (m_shuttingDown) return;
    m_joinCode = code.trimmed();
    emit changed();
}

void DeskShareController::loadPreferences()
{
    if (m_preferencesLoaded) return;
    m_preferencesLoaded = true;
    if (!m_preferences) return;
    const auto values = m_preferences->load();
    if (!values.error.isEmpty()) {
        reportPreferencesError(values.error);
        return;
    }
    if (m_serverAddress.isEmpty() && !values.serverAddress.isEmpty())
        m_serverAddress = values.serverAddress;
    if (!values.profile.isEmpty()) m_profile = values.profile;
    m_preferencesError.clear();
}

void DeskShareController::schedulePreferencesSave()
{
    if (!m_preferences || !m_preferencesLoaded) return;
    m_preferencesSaveTimer.start();
}

void DeskShareController::flushPreferences()
{
    if (!hasDirtyPreferences() || !m_preferences || !m_preferencesLoaded) return;
    // Only the fields this controller edited are merged. The dirty flags stay
    // set on failure so the same edit is retried instead of being lost, and they
    // are cleared only for the exact value that was written.
    Preferences::Change change;
    change.serverAddress = m_serverAddress;
    change.profile = m_profile;
    change.serverAddressDirty = m_dirtyAddress;
    change.profileDirty = m_dirtyProfile;
    Preferences::Values merged;
    QString error;
    if (!m_preferences->save(change, &merged, &error)) {
        reportPreferencesError(error);
        return;
    }
    if (m_dirtyAddress && m_serverAddress == change.serverAddress) m_dirtyAddress = false;
    if (m_dirtyProfile && m_profile == change.profile) m_dirtyProfile = false;
    if (!m_preferencesError.isEmpty()) {
        m_preferencesError.clear();
        emit changed();
    }
}

void DeskShareController::reportPreferencesError(const QString &message)
{
    const auto text = QStringLiteral("偏好保存失败：%1（本次运行仍可使用，但重启后可能丢失）").arg(message);
    if (m_preferencesError == text) return;
    m_preferencesError = text;
    m_details = text;
    emit changed();
}

void DeskShareController::initialize()
{
    if (m_shuttingDown) return;
    loadPreferences();
    refreshSources();
}

void DeskShareController::setPreview(media::VideoPreview *preview)
{
    m_preview = preview;
}

void DeskShareController::setSourceValue(const QString &value)
{
    if (m_shuttingDown || busy()) return;
    if (refuseWhileReleasing()) return;
    if (capturing() && !m_publisherRoom) return;
    if (value == m_sourceValue) return;
    bool known = value.isEmpty();
    for (const auto &entry : m_sources)
        if (entry.toMap().value(QStringLiteral("value")).toString() == value) known = true;
    m_sourceValue = value;
    m_sourceMissing = !known;
    clearError();
    if (!known)
        m_status = QStringLiteral("所选来源已不可用，请重新选择（不会自动切换到其他来源）。");
    else if (value.isEmpty())
        m_status = QStringLiteral("请选择屏幕或窗口来源。");
    else
        m_status = QStringLiteral("来源已选择。");
    if (m_connected && !capturing() && !m_publisherRoom && !m_viewerRole)
        setPhase(known && !value.isEmpty() ? QStringLiteral("ready")
                                           : QStringLiteral("connected"));
    emit changed();
    if (m_publisherRoom && capturing() && known) restartPublishing();
}

void DeskShareController::setProfile(const QString &profile)
{
    if (m_shuttingDown || busy()) return;
    if (profile != QStringLiteral("720p30") && profile != QStringLiteral("1080p30")) return;
    if (profile == m_profile) return;
    if (refuseWhileReleasing()) return;
    if (capturing() && !m_publisherRoom) return;
    m_profile = profile;
    m_dirtyProfile = true;
    schedulePreferencesSave();
    if (m_state == QStringLiteral("idle"))
        m_status = QStringLiteral("已选择 %1 档位。").arg(profile);
    emit changed();
    if (m_publisherRoom && capturing()) restartPublishing();
}

void DeskShareController::setCodec(const QString &codec)
{
    if (m_shuttingDown || busy()) return;
    if (codec != QStringLiteral("h264") && codec != QStringLiteral("bgra-tiles")) return;
    if (codec == m_codec) return;
    if (refuseWhileReleasing()) return;
    if (capturing() && !m_publisherRoom) return;
    m_codec = codec;
    m_status = codec == QStringLiteral("bgra-tiles")
        ? QStringLiteral("文字桌面：无损更新变化区域，带宽不足时轮换刷新。")
        : QStringLiteral("视频：使用 H.264 编码，适合持续运动画面。");
    emit changed();
    if (m_publisherRoom && capturing()) restartPublishing();
}

void DeskShareController::refreshSources()
{
    if (!m_session || m_shuttingDown) return;
    const auto epoch = ++m_sourceEpoch;
    m_session->sources(this, [this, epoch](media::Result<media::SourceSnapshot> result) {
        if (epoch != m_sourceEpoch || m_shuttingDown) return;
        if (!result.ok()) {
            if (m_phase == QStringLiteral("disconnected") || m_phase == QStringLiteral("connected")
                || m_phase == QStringLiteral("ready")) {
                m_status = QStringLiteral("无法枚举屏幕和窗口：%1").arg(result.error.message);
                setError(QStringLiteral("无法枚举屏幕和窗口：%1").arg(result.error.message),
                         QStringLiteral("确认媒体插件已加载并授予屏幕捕获权限后重试刷新。"));
                emit changed();
            }
            return;
        }
        QVariantList list;
        const bool hadSelection = !m_sourceValue.isEmpty();
        bool found = m_sourceValue.isEmpty();
        for (const auto &source : result.value.sources) {
            if (source.kind != QStringLiteral("screen") && source.kind != QStringLiteral("window"))
                continue;
            const auto value = source.kind + QLatin1Char(':') + source.id;
            const auto label = (source.kind == QStringLiteral("screen") ? QStringLiteral("屏幕")
                                                                        : QStringLiteral("窗口")) +
                               QStringLiteral(" · ") + source.description;
            list.append(QVariantMap{{QStringLiteral("value"), value},
                                    {QStringLiteral("kind"), source.kind},
                                    {QStringLiteral("description"), label}});
            if (value == m_sourceValue) found = true;
        }
        m_sources = list;
        m_sourceMissing = !found;
        if (!found && !capturing() && !m_publisherRoom && !m_viewerRole) m_sourceValue.clear();
        if (found) m_sourceMissingSinceMs = 0;
        else if (hadSelection && !capturing()) m_sourceMissingSinceMs = 0;
        if (m_phase == QStringLiteral("disconnected") || m_phase == QStringLiteral("connected")
            || m_phase == QStringLiteral("ready")) {
            if (hadSelection && !found)
                m_status =
                    QStringLiteral("所选来源已不可用，请重新选择（不会自动切换到其他来源）。");
            else if (list.isEmpty())
                m_status = QStringLiteral("当前没有可共享的屏幕或窗口。");
            else if (m_sourceValue.isEmpty())
                m_status = QStringLiteral("请选择屏幕或窗口来源。");
            else
                m_status = QStringLiteral("来源已选择。");
            if (m_connected)
                setPhase(m_sourceValue.isEmpty() ? QStringLiteral("connected")
                                                 : QStringLiteral("ready"));
        }
        emit changed();
    });
}

void DeskShareController::connectServer()
{
    if (m_shuttingDown || m_connected || isTransient()) return;
    QString text = m_serverAddress.trimmed();
    if (text.isEmpty()) {
        setError(QStringLiteral("请先填写服务器地址。"),
                 QStringLiteral("填写形如 192.168.1.10:18100 的地址，再点击“连接”。"));
        m_status = m_errorText;
        emit changed();
        return;
    }
    quint16 port = DefaultPort;
    const auto colon = text.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
        bool ok = false;
        const auto parsed = text.mid(colon + 1).toUShort(&ok);
        if (ok && parsed > 0) {
            port = parsed;
            text = text.left(colon);
        }
    }
    m_serverPort = port;
    ++m_lifecycle;
    m_networkState = QStringLiteral("connecting");
    m_state = QStringLiteral("idle");
    clearError();
    setOperation(QStringLiteral("connect"));
    setPhase(QStringLiteral("connecting"));
    m_status = QStringLiteral("正在连接 %1:%2 …（只握手，不采集）").arg(text).arg(port);
    m_network->connectToServer(text, port);
    emit changed();
}

void DeskShareController::disconnectServer()
{
    if (m_shuttingDown) return;
    if (m_operation == QStringLiteral("connect") || m_operation == QStringLiteral("create")
        || m_operation == QStringLiteral("join") || m_operation == QStringLiteral("start")) {
        cancelOperation();
        return;
    }
    if (busy()) return;
    if (m_publisherRoom || m_viewerRole || capturing()) {
        const bool leave = m_viewerRole;
        beginRoomStop(leave, QStringLiteral("已断开服务端连接。"), QString(),
                      [this](bool ok) {
                          if (!ok) return;
                          if (m_network) m_network->disconnectFromServer(
                              QStringLiteral("本地断开连接"));
                          m_connected = false;
                          m_networkState = QStringLiteral("disconnected");
                          ++m_lifecycle;
                          clearError();
                          m_status = QStringLiteral("已断开服务端连接。");
                          setPhase(QStringLiteral("disconnected"));
                          emit changed();
                      });
        return;
    }
    if (m_network) m_network->disconnectFromServer(QStringLiteral("本地断开连接"));
    m_connected = false;
    m_networkState = QStringLiteral("disconnected");
    ++m_lifecycle;
    clearError();
    m_status = QStringLiteral("已断开服务端连接。");
    setPhase(QStringLiteral("disconnected"));
    emit changed();
}

void DeskShareController::onConnected(quint64 sessionId, quint16 udpPort)
{
    if (m_shuttingDown || m_connected) return;
    m_observedStream.clear();
    m_viewerFailure.clear();
    m_connected = true;
    m_sessionId = sessionId;
    m_udpPort = udpPort;
    m_networkState = QStringLiteral("connected");
    clearError();
    const bool wasConnect = m_operation == QStringLiteral("connect");
    if (wasConnect) clearOperation();
    m_status = QStringLiteral("已连接服务端（会话 %1，UDP 端口 %2）。").arg(sessionId).arg(udpPort);
    setPhase(m_sourceValue.isEmpty() || m_sourceMissing ? QStringLiteral("connected")
                                                        : QStringLiteral("ready"));
    emit changed();
    if (m_pendingCreate) {
        m_pendingCreate = false;
        createRoom();
    } else if (m_pendingJoin) {
        m_pendingJoin = false;
        joinRoom();
    }
}

void DeskShareController::createRoom()
{
    if (m_shuttingDown || busy() || m_publisherRoom || m_viewerRole) return;
    if (refuseWhileReleasing()) return;
    if (!m_connected) {
        if (m_networkState == QStringLiteral("connecting")) {
            m_pendingCreate = true;
            return;
        }
        m_pendingCreate = true;
        connectServer();
        return;
    }
    if (m_sourceValue.isEmpty() || m_sourceMissing) {
        setError(m_sourceValue.isEmpty() ? QStringLiteral("请先选择屏幕或窗口来源。")
                                         : QStringLiteral("所选来源已不可用，请重新选择。"),
                 QStringLiteral("刷新来源并重新选择后再开始共享。"));
        m_status = m_errorText;
        emit changed();
        return;
    }
    m_pendingCreate = false;
    clearError();
    setOperation(QStringLiteral("create"));
    m_networkState = QStringLiteral("creating");
    setPhase(QStringLiteral("creating"));
    m_status = QStringLiteral("正在创建共享房间…");
    m_network->createRoom(m_profile);
    emit changed();
}

void DeskShareController::startSharing()
{
    if (m_shuttingDown || busy() || m_publisherRoom || m_viewerRole || capturing()) return;
    if (refuseWhileReleasing()) return;
    if (!m_connected) {
        setError(QStringLiteral("尚未连接服务端。"),
                 QStringLiteral("先点击“连接”完成握手，再点击“开始共享”。"));
        m_status = m_errorText;
        emit changed();
        return;
    }
    if (m_sourceValue.isEmpty() || m_sourceMissing) {
        setError(m_sourceValue.isEmpty() ? QStringLiteral("请先选择屏幕或窗口来源。")
                                         : QStringLiteral("所选来源已不可用，请重新选择。"),
                 QStringLiteral("刷新来源并重新选择后再开始共享。"));
        m_status = m_errorText;
        emit changed();
        return;
    }
    clearError();
    setOperation(QStringLiteral("create"));
    m_networkState = QStringLiteral("creating");
    setPhase(QStringLiteral("creating"));
    m_status = QStringLiteral("正在创建共享房间…");
    m_network->createRoom(m_profile);
    emit changed();
}

void DeskShareController::joinRoom()
{
    if (m_shuttingDown || busy() || m_publisherRoom || m_viewerRole) return;
    if (refuseWhileReleasing()) return;
    if (!m_connected) {
        if (m_networkState == QStringLiteral("connecting")) {
            m_pendingJoin = true;
            return;
        }
        m_pendingJoin = true;
        connectServer();
        return;
    }
    const auto code = m_joinCode.trimmed();
    if (!isValidRoomCode(code)) {
        setError(QStringLiteral("请输入有效的八位房间号。"),
                 QStringLiteral("房间号是八位十进制数字（含前导零）。"));
        m_status = m_errorText;
        emit changed();
        return;
    }
    m_pendingJoin = false;
    clearError();
    setOperation(QStringLiteral("join"));
    m_networkState = QStringLiteral("joining");
    setPhase(QStringLiteral("joining"));
    m_status = QStringLiteral("正在加入房间…");
    m_network->joinRoom(code);
    emit changed();
}

void DeskShareController::joinRoomExplicit()
{
    if (!m_connected) {
        setError(QStringLiteral("尚未连接服务端。"),
                 QStringLiteral("先点击“连接”完成握手，再点击“加入观看”。"));
        m_status = m_errorText;
        emit changed();
        return;
    }
    joinRoom();
}

void DeskShareController::onRoomCreated(quint64 roomId, const QString &code, const QString &profile,
                                        const QByteArray &token, bool hasFormat,
                                        const StreamFormat &format)
{
    Q_UNUSED(hasFormat);
    Q_UNUSED(format);
    if (m_shuttingDown) return;
    const bool owns = m_operation == QStringLiteral("create")
        || (m_operation.isEmpty() && !m_connected && !m_stopping && !m_publisherRoom
            && !m_viewerRole);
    if (!owns) {
        // No live create operation owns this answer. Never turn an unexpected
        // room into a capture session; close it instead.
        if (m_network) m_network->stopRoom(QStringLiteral("未请求的房间响应"));
        return;
    }
    clearOperation();
    setOperation(QStringLiteral("start"));
    m_publisherRoom = true;
    m_viewerRole = false;
    m_roomId = roomId;
    m_roomCode = code;
    m_roomToken = token;
    m_viewerCount = 0;
    m_networkState = QStringLiteral("publishing");
    clearError();
    m_status = QStringLiteral("房间已创建（档位 %1），正在启动采集…").arg(profile);
    setPhase(QStringLiteral("startingShare"));
    emit changed();
    beginCapture();
}

void DeskShareController::onRoomJoined(quint64 roomId, const QString &code, const QString &profile,
                                       const QByteArray &token, bool hasFormat,
                                       const StreamFormat &format)
{
    Q_UNUSED(profile);
    if (m_shuttingDown) return;
    const bool owns = m_operation == QStringLiteral("join")
        || (m_operation.isEmpty() && !m_connected && !m_stopping && !m_publisherRoom
            && !m_viewerRole);
    if (!owns) {
        if (m_network) m_network->leaveRoom(QStringLiteral("未请求的房间响应"));
        return;
    }
    clearOperation();
    m_viewerRole = true;
    m_publisherRoom = false;
    m_roomId = roomId;
    m_roomCode = code;
    m_roomToken = token;
    m_networkState = QStringLiteral("waiting");
    clearError();
    if (hasFormat && format.streamEpoch > 0) {
        m_status = QStringLiteral("已加入房间，正在准备接收轨道…");
        setPhase(QStringLiteral("waitingFirstFrame"));
        emit changed();
        beginViewing(format);
        return;
    }
    m_status = QStringLiteral("已加入房间，等待共享者开始共享…");
    setPhase(QStringLiteral("viewerWaiting"));
    emit changed();
}

void DeskShareController::onStreamConfig(const StreamFormat &format)
{
    if (m_shuttingDown || m_stopping || !m_viewerRole) return;
    if (format.streamEpoch == 0) return;
    if (format.streamEpoch == m_viewEpoch && m_receiveReady) return;
    beginViewing(format);
}

void DeskShareController::beginViewing(const StreamFormat &format)
{
    if (!m_session || m_shuttingDown || !m_viewerRole) return;
    m_observedStream = {{QStringLiteral("generation"), QString::number(++m_observationGeneration)},
                        {QStringLiteral("sessionId"), QString::number(m_sessionId)},
                        {QStringLiteral("roomId"), QString::number(m_roomId)},
                        {QStringLiteral("streamEpoch"), QString::number(format.streamEpoch)}};
    m_viewerFailure.clear();
    teardownReceiveTrack();
    m_viewFormat = format;
    m_viewEpoch = format.streamEpoch;
    m_viewFailed = false;
    m_gapAfterDecodedFrame = false;
    m_submitFailures = 0;
    m_decodedFrames = 0;
    m_frameWidth = format.width;
    m_frameHeight = format.height;
    m_viewStartMs = nowMs();
    m_lastNetworkPacketMs = 0;
    m_lastDecodedMs = 0;
    m_acceptingFrames = true;
    if (m_preview) m_preview->clear();
    m_network->beginReceive(format.streamEpoch);
    m_network->clearUpload();
    m_networkState = QStringLiteral("viewing");
    m_status = QStringLiteral("正在创建并启动本地接收轨道…");
    setPhase(QStringLiteral("waitingFirstFrame"));
    emit changed();
    if (m_sessionOpen) {
        createViewerTrack(format);
        return;
    }
    if (m_opening) return;
    m_opening = true;
    const auto lifecycle = m_lifecycle;
    m_session->open(this, [this, lifecycle](media::Result<bool> opened) {
        m_opening = false;
        if (m_shuttingDown || lifecycle != m_lifecycle) return;
        if (!opened.ok()) {
            endWatching(QStringLiteral("无法打开媒体数据通道：%1").arg(opened.error.message),
                        true);
            return;
        }
        m_sessionOpen = true;
        if (!m_viewerRole || m_viewEpoch == 0) return;
        createViewerTrack(m_viewFormat);
    });
}

void DeskShareController::createViewerTrack(const StreamFormat &format)
{
    const auto epoch = m_viewEpoch;
    const auto lifecycle = m_lifecycle;
    m_session->createTrack(receiveConfig(format), this,
                           [this, epoch, lifecycle](media::Result<media::TrackDescriptor> result) {
                               if (m_shuttingDown || !m_viewerRole || epoch != m_viewEpoch
                                   || lifecycle != m_lifecycle) {
                                   if (result.ok()) requestTrackRelease(result.value.id, false);
                                   return;
                               }
                               if (!result.ok()) {
                                   failRoom(QStringLiteral("创建接收轨道失败：%1")
                                                .arg(result.error.message),
                                            QStringLiteral("重新点击“加入观看”以重试。"), QStringLiteral("RECEIVER_CREATE_FAILED"));
                                   return;
                               }
                               m_receiveId = result.value.id;
                               m_receiveDescriptor = result.value;
                               m_session->start(m_receiveId, this,
                                                [this, epoch, lifecycle](
                                                    media::Result<bool> started) {
                                                    if (m_shuttingDown || !m_viewerRole
                                                        || epoch != m_viewEpoch
                                                        || lifecycle != m_lifecycle)
                                                        return;
                                                    if (!started.ok()) {
                                                        failRoom(
                                                            QStringLiteral("启动接收轨道失败：%1")
                                                                .arg(started.error.message),
                                                            QStringLiteral("重新点击“加入观看”以重试。"), QStringLiteral("RECEIVER_START_FAILED"));
                                                        return;
                                                    }
                                                    m_receiveReady = true;
                                                    m_viewStartMs = nowMs();
                                                    // A new viewing session starts a new
                                                    // feedback cadence: send the first report
                                                    // immediately and pace the rest from its
                                                    // acknowledgement.
                                                    m_feedbackPending = false;
                                                    m_feedbackSentMs = 0;
                                                    m_feedbackAckMs = 0;
                                                    m_network->streamReady(epoch);
                                                    requestRemoteKeyFrame();
                                                    m_status = QStringLiteral(
                                                        "接收轨道已就绪，等待首个关键帧…");
                                                    setPhase(QStringLiteral("waitingFirstFrame"));
                                                    emit changed();
                                                });
                           });
}

void DeskShareController::teardownReceiveTrack()
{
    const auto receive = m_receiveId;
    m_receiveId.clear();
    m_receiveDescriptor = {};
    m_receiveReady = false;
    m_receiveStarting = false;
    if (!receive.isEmpty()) requestTrackRelease(receive, false);
}

void DeskShareController::teardownSendTrack()
{
    const auto send = m_sendId;
    m_sendId.clear();
    if (!send.isEmpty()) requestTrackRelease(send, true);
}

void DeskShareController::clearTrackState()
{
    m_sendId.clear();
    m_receiveId.clear();
    m_receiveDescriptor = {};
    m_receiveReady = false;
    m_receiveStarting = false;
    m_initializationData.clear();
}

void DeskShareController::clearRoomIdentity()
{
    m_roomId = 0;
    m_roomCode.clear();
    m_roomToken.clear();
    m_viewerCount = 0;
    m_viewFormat = {};
    m_viewEpoch = 0;
    m_publishEpoch = 0;
    m_configurePending = false;
    m_awaitingKeyFrame = false;
    m_nextWireSequence = 0;
}

void DeskShareController::endWatching(const QString &message, bool leaveRemote)
{
    if (m_shuttingDown) return;
    recordViewerFailure(QStringLiteral("MEDIA_CHANNEL_FAILED"), message);
    ++m_lifecycle;
    ++m_viewEpoch;
    m_acceptingFrames = false;
    teardownReceiveTrack();
    if (leaveRemote && m_network) m_network->leaveRoom(message);
    m_network->clearReceive();
    m_viewerRole = false;
    clearRoomIdentity();
    m_networkState = m_connected ? QStringLiteral("connected") : QStringLiteral("disconnected");
    m_state = QStringLiteral("failed");
    setError(message, QStringLiteral("核对房号与服务端后重新点击“加入观看”。"));
    m_status = message;
    m_lastNetworkError = message;
    if (m_preview) m_preview->clear();
    setPhase(m_connected ? QStringLiteral("failed") : QStringLiteral("disconnected"));
    emit changed();
}

void DeskShareController::endPublishing(const QString &message, bool stopRemote)
{
    if (m_shuttingDown) return;
    ++m_lifecycle;
    ++m_epoch;
    m_acceptingFrames = false;
    if (stopRemote && m_network) {
        m_network->clearUpload();
        m_network->stopRoom(message);
    }
    m_publisherRoom = false;
    clearRoomIdentity();
    teardownSendTrack();
    teardownReceiveTrack();
    m_networkState = m_connected ? QStringLiteral("connected") : QStringLiteral("disconnected");
    m_state = QStringLiteral("failed");
    setError(message, QStringLiteral("重新选择来源后再次点击“开始共享”。"));
    m_status = message;
    m_lastNetworkError = message;
    if (m_preview) m_preview->clear();
    setPhase(m_connected ? QStringLiteral("failed") : QStringLiteral("disconnected"));
    emit changed();
}

void DeskShareController::recordViewerFailure(const QString &reasonCode, const QString &message)
{
    if (!m_viewerRole || m_observedStream.isEmpty() || !m_viewerFailure.isEmpty()) return;
    m_viewerFailure = {{QStringLiteral("stream"), m_observedStream},
                       {QStringLiteral("reasonCode"), reasonCode},
                       {QStringLiteral("message"), message},
                       {QStringLiteral("occurredEpochMs"), double(QDateTime::currentMSecsSinceEpoch())}};
}

void DeskShareController::failRoom(const QString &message, const QString &nextAction, const QString &reasonCode)
{
    if (m_shuttingDown || m_stopping) return;
    recordViewerFailure(reasonCode, message);
    setError(message, nextAction);
    m_lastNetworkError = message;
    const bool leave = m_viewerRole;
    const bool hadRoom = m_publisherRoom || m_viewerRole;
    if (!hadRoom) {
        fail(message);
        return;
    }
    beginRoomStop(leave, message, message, [this, message](bool ok) {
        if (!ok) return;
        m_state = QStringLiteral("failed");
        m_status = message;
        setPhase(m_connected ? QStringLiteral("failed") : QStringLiteral("disconnected"));
        emit changed();
    });
    m_status = message;
    emit changed();
}

void DeskShareController::startCapture()
{
    if (!m_session || m_shuttingDown || busy() || capturing()) return;
    if (refuseWhileReleasing()) return;
    if (m_sourceValue.isEmpty() || m_sourceMissing) {
        setError(m_sourceValue.isEmpty() ? QStringLiteral("请先选择屏幕或窗口来源。")
                                         : QStringLiteral("所选来源已不可用，请重新选择。"),
                 QStringLiteral("刷新来源并重新选择。"));
        m_status = m_errorText;
        emit changed();
        return;
    }
    beginCapture();
}

void DeskShareController::beginCapture()
{
    if (!m_session || m_shuttingDown) return;
    ++m_epoch;
    ++m_lifecycle;
    clearTrackState();
    m_capturedFrames = 0;
    m_decodedFrames = 0;
    m_dropped = 0;
    m_publishEpoch = 0;
    m_configurePending = false;
    m_awaitingKeyFrame = false;
    m_nextWireSequence = 0;
    const auto profile = profileFor(m_profile);
    m_frameWidth = profile.width;
    m_frameHeight = profile.height;
    m_lastPacketMs = 0;
    m_lastDecodedMs = 0;
    m_startRequestedMs = nowMs();
    m_lastKeyRequestMs = 0;
    m_acceptingFrames = true;
    m_details.clear();
    clearError();
    m_state = QStringLiteral("starting");
    setPhase(m_publisherRoom ? QStringLiteral("startingShare") : QStringLiteral("previewing"));
    m_status = QStringLiteral("正在打开媒体数据通道并准备采集…");
    if (m_preview) m_preview->clear();
    emit changed();
    if (m_sessionOpen) {
        startTracks();
        return;
    }
    if (m_opening) return;
    m_opening = true;
    const auto lifecycle = m_lifecycle;
    m_session->open(this, [this, lifecycle](media::Result<bool> result) {
        m_opening = false;
        if (m_shuttingDown || lifecycle != m_lifecycle) return;
        if (!result.ok()) {
            fail(QStringLiteral("无法打开媒体数据通道：%1").arg(result.error.message));
            return;
        }
        m_sessionOpen = true;
        if (m_state != QStringLiteral("starting") || !m_acceptingFrames) return;
        startTracks();
    });
}

void DeskShareController::restartPublishing()
{
    if (!m_session || m_shuttingDown) return;
    ++m_lifecycle;
    const auto send = m_sendId;
    const auto receive = m_receiveId;
    m_sendId.clear();
    m_receiveId.clear();
    m_receiveDescriptor = {};
    m_receiveReady = false;
    m_receiveStarting = false;
    m_initializationData.clear();
    if (!send.isEmpty()) requestTrackRelease(send, true);
    if (!receive.isEmpty()) requestTrackRelease(receive, false);
    m_network->clearUpload();
    m_network->clearReceive();
    m_publishEpoch = 0;
    m_configurePending = false;
    m_awaitingKeyFrame = false;
    m_nextWireSequence = 0;
    beginCapture();
}

void DeskShareController::startTracks()
{
    if (!m_session || m_shuttingDown) return;
    const auto epoch = m_epoch;
    const auto lifecycle = m_lifecycle;
    const auto source = m_sourceValue;
    m_session->createTrack(videoConfig(true, source, {}), this,
                           [this, epoch, lifecycle](media::Result<media::TrackDescriptor> result) {
                               if (epoch != m_epoch || lifecycle != m_lifecycle
                                   || m_shuttingDown) {
                                   if (result.ok()) requestTrackRelease(result.value.id, true);
                                   return;
                               }
                               if (!result.ok()) {
                                   fail(QStringLiteral("创建采集轨道失败：%1")
                                            .arg(result.error.message));
                                   return;
                               }
                               m_sendId = result.value.id;
                               m_session->start(m_sendId, this,
                                                [this, epoch, lifecycle](
                                                    media::Result<bool> result) {
                                                    if (epoch != m_epoch
                                                        || lifecycle != m_lifecycle
                                                        || m_shuttingDown)
                                                        return;
                                                    if (!result.ok()) {
                                                        fail(QStringLiteral("启动采集失败：%1")
                                                                 .arg(result.error.message));
                                                        return;
                                                    }
                                                    clearOperation();
                                                    if (m_state == QStringLiteral("starting")) {
                                                        m_status = QStringLiteral(
                                                            "采集已启动，等待首个编码画面…");
                                                        if (m_publisherRoom)
                                                            setPhase(QStringLiteral("waitingSource"));
                                                        emit changed();
                                                    }
                                                });
                           });
}

void DeskShareController::publishPacket(const media::EncodedPacket &packet)
{
    if (!m_network || m_shuttingDown || m_stopping || !m_publisherRoom || !m_acceptingFrames)
        return;
    if (m_publishEpoch == 0) {
        if (m_configurePending) {
            ++m_dropped;
            return;
        }
        const auto profile = profileFor(m_profile);
        StreamFormat format;
        format.codec = m_codec;
        format.width = profile.width;
        format.height = profile.height;
        format.fps = FrameRate;
        format.bitrate = profile.bitrate;
        format.initializationData = m_codec == QStringLiteral("bgra-tiles")
                                        ? QByteArray()
                                        : packet.initializationData;
        m_configurePending = true;
        m_network->configureStream(format);
        ++m_dropped;
        m_status = QStringLiteral("已取得首个编码包，正在协商视频流格式…");
        emit changed();
        return;
    }
    if (m_awaitingKeyFrame) {
        if (!packet.keyFrame) {
            ++m_dropped;
            return;
        }
        m_awaitingKeyFrame = false;
    }
    VideoPacket wire;
    wire.streamEpoch = m_publishEpoch;
    wire.sequence = m_nextWireSequence++;
    wire.pts = packet.pts;
    wire.dts = packet.dts.value_or(0);
    wire.duration = quint32(qMax<qint64>(0, packet.duration));
    if (packet.keyFrame) wire.flags |= quint16(MediaFlag::KeyFrame);
    if (packet.discontinuity) wire.flags |= quint16(MediaFlag::Discontinuity);
    if (packet.dts) wire.flags |= quint16(MediaFlag::HasDts);
    wire.data = packet.data;
    if (!m_network->enqueueVideo(wire)) {
        ++m_dropped;
        ++m_uploadDropped;
        m_awaitingKeyFrame = true;
        requestLocalKeyFrame();
    }
}

void DeskShareController::packetArrived(media::EncodedPacket packet)
{
    if (m_shuttingDown || m_stopping || !m_acceptingFrames || !m_session) return;
    if (m_sendId.isEmpty() || packet.track != m_sendId) return;
    m_lastPacketMs = nowMs();
    ++m_capturedFrames;
    if (m_publisherRoom) {
        if (m_phase == QStringLiteral("startingShare") || m_phase == QStringLiteral("waitingSource"))
            setPhase(QStringLiteral("waitingFirstFrame"));
        publishPacket(packet);
    }
    if (m_receiveReady) {
        submitPacket(std::move(packet));
        return;
    }
    if (m_receiveStarting) {
        ++m_dropped;
        return;
    }
    ++m_dropped;
    m_initializationData = packet.initializationData;
    if (qEnvironmentVariableIntValue("ZTO_DESKTOP_SHARE_DIAGNOSTICS"))
        qInfo() << "loopback receive preparation" << "atMs" << nowMs()
                << "initialPacketPts" << packet.pts;
    beginLoopback();
}

void DeskShareController::beginLoopback()
{
    const auto epoch = m_epoch;
    const auto lifecycle = m_lifecycle;
    m_receiveStarting = true;
    m_session->createTrack(videoConfig(false, QString(), m_initializationData), this,
                           [this, epoch, lifecycle](media::Result<media::TrackDescriptor> result) {
                               if (m_shuttingDown || epoch != m_epoch || lifecycle != m_lifecycle) {
                                   if (result.ok()) requestTrackRelease(result.value.id, false);
                                   return;
                               }
                               if (!result.ok()) {
                                   m_receiveStarting = false;
                                   fail(QStringLiteral("创建回送接收轨道失败：%1")
                                            .arg(result.error.message));
                                   return;
                               }
                               m_receiveId = result.value.id;
                               m_receiveDescriptor = result.value;
                               m_session->start(m_receiveId, this,
                                                [this, epoch, lifecycle](
                                                    media::Result<bool> result) {
                                                    if (m_shuttingDown || epoch != m_epoch
                                                        || lifecycle != m_lifecycle)
                                                        return;
                                                    if (!result.ok()) {
                                                        m_receiveStarting = false;
                                                        fail(QStringLiteral(
                                                                 "启动回送接收轨道失败：%1")
                                                                 .arg(result.error.message));
                                                        return;
                                                    }
                                                    m_receiveReady = true;
                                                    m_receiveStarting = false;
                                                    if (m_session && !m_sendId.isEmpty())
                                                        m_session->requestKeyFrame(m_sendId, this);
                                                    m_status = QStringLiteral(
                                                        "回送轨道已就绪，等待首个解码画面…");
                                                    if (m_publisherRoom)
                                                        setPhase(QStringLiteral("waitingFirstFrame"));
                                                    emit changed();
                                                });
                           });
}

void DeskShareController::submitPacket(media::EncodedPacket packet)
{
    if (!m_session || m_receiveId.isEmpty() || !m_acceptingFrames) return;
    if (!m_session->submitTo(m_receiveDescriptor, std::move(packet))) ++m_dropped;
}

void DeskShareController::drainNetworkVideo()
{
    if (!m_network || m_shuttingDown || m_stopping) return;
    QList<VideoPacket> packets;
    m_network->takeVideo(packets);
    if (packets.isEmpty()) return;
    if (!m_viewerRole || !m_receiveReady || m_receiveId.isEmpty() || !m_session
        || !m_acceptingFrames) return;
    m_lastNetworkPacketMs = nowMs();
    bool submitted = false;
    for (auto &wire : packets) {
        if (wire.streamEpoch != m_viewEpoch) continue;
        media::EncodedPacket packet;
        packet.track = m_receiveDescriptor.id;
        packet.codec = m_viewFormat.codec;
        packet.generation = m_receiveDescriptor.generation;
        packet.configuration = m_receiveDescriptor.configuration;
        packet.sequence = wire.sequence;
        packet.pts = wire.pts;
        if (wire.flags & quint16(MediaFlag::HasDts)) packet.dts = wire.dts;
        packet.duration = wire.duration;
        packet.timeBase = {1, 1000000};
        packet.keyFrame = (wire.flags & quint16(MediaFlag::KeyFrame)) != 0;
        packet.discontinuity = (wire.flags & quint16(MediaFlag::Discontinuity)) != 0;
        packet.initializationData = m_viewFormat.initializationData;
        packet.data = std::move(wire.data);
        if (m_session->submitTo(m_receiveDescriptor, std::move(packet))) {
            ++m_networkSubmitted;
            m_submitFailures = 0;
            submitted = true;
        } else {
            ++m_dropped;
            ++m_submitFailures;
            if (m_submitFailures >= 10) {
                failRoom(QStringLiteral("本地接收轨道持续拒绝编码包，已退出本次观看。"),
                         QStringLiteral("重新点击“加入观看”以重试。"), QStringLiteral("RECEIVER_SUBMIT_FAILED"));
                return;
            }
            m_network->resyncReceive();
            requestRemoteKeyFrame();
            return;
        }
    }
    if (submitted) {
        if (m_viewerRole) updateNetworkStatus();
        emit changed();
    }
}

void DeskShareController::requestLocalKeyFrame()
{
    if (!m_session || m_sendId.isEmpty() || m_shuttingDown || m_stopping) return;
    const auto now = nowMs();
    if (m_lastKeyRequestMs && now - m_lastKeyRequestMs < 1000) return;
    m_lastKeyRequestMs = now;
    m_session->requestKeyFrame(m_sendId, this);
}

void DeskShareController::requestRemoteKeyFrame()
{
    if (!m_network || m_viewEpoch == 0 || m_shuttingDown || m_stopping) return;
    const auto now = nowMs();
    if (m_lastRemoteKeyRequestMs && now - m_lastRemoteKeyRequestMs < 1000) return;
    m_lastRemoteKeyRequestMs = now;
    m_network->requestKeyframe(m_viewEpoch);
}

void DeskShareController::frameArrived(const media::FrameView &frame)
{
    if (m_shuttingDown || m_stopping || !m_acceptingFrames) return;
    if (m_receiveId.isEmpty() || frame.track != m_receiveId || !frame.decoded) return;
    if (m_preview) m_preview->present(frame);
    m_lastDecodedMs = nowMs();
    m_gapAfterDecodedFrame = false;
    ++m_decodedFrames;
    m_frameWidth = frame.video.width;
    m_frameHeight = frame.video.height;
    if (m_viewerRole) {
        if (m_phase != QStringLiteral("viewing")) setPhase(QStringLiteral("viewing"));
        updateNetworkStatus();
        emit changed();
        return;
    }
    if (m_publisherRoom && m_phase != QStringLiteral("sharing")) setPhase(QStringLiteral("sharing"));
    if (m_state == QStringLiteral("starting") || m_state == QStringLiteral("waiting")
        || m_state == QStringLiteral("interrupted")) {
        m_state = QStringLiteral("live");
        m_status = liveStatus();
        emit changed();
    }
}

void DeskShareController::updateNetworkStatus()
{
    if (!m_viewerRole) return;
    if (!m_receiveReady) {
        m_status = QStringLiteral("正在准备本地接收轨道…");
        setPhase(QStringLiteral("waitingFirstFrame"));
        return;
    }
    const auto now = nowMs();
    const auto label = QStringLiteral("%1×%2 · %3 fps · %4 Mbps")
                           .arg(m_viewFormat.width)
                           .arg(m_viewFormat.height)
                           .arg(m_viewFormat.fps)
                           .arg(m_viewFormat.bitrate / 1000000.0, 0, 'f', 1);
    if (m_decodedFrames == 0) {
        if (m_viewFailed) return;
        if (now - m_viewStartMs >= FirstFrameTimeoutMs) {
            m_viewFailed = true;
            failRoom(QStringLiteral("10 秒内没有收到可解码的首帧，已退出本次观看。"),
                     QStringLiteral("确认共享者仍在共享后重新加入。"), QStringLiteral("FIRST_FRAME_TIMEOUT"));
            return;
        }
        m_status = QStringLiteral("接收轨道已就绪，等待首个关键帧…");
        setPhase(QStringLiteral("waitingFirstFrame"));
        return;
    }
    const auto sinceFrame = now - m_lastDecodedMs;
    // Silence is a valid result of event-driven capture. Only positive evidence
    // of lost media or undecodable incoming packets makes it an interruption;
    // endpoint/control/source failures still take their separate terminal path.
    // A last packet can be a heartbeat, a delayed duplicate or a refresh. Its
    // arrival alone says nothing about new source content. Only an observed
    // unresolved gap is interruption evidence; decoder/source/endpoint errors
    // still end the session through their structured failure paths.
    const bool mediaStalled = m_gapAfterDecodedFrame;
    if (sinceFrame >= InterruptedAfterMs && mediaStalled) {
        m_networkState = QStringLiteral("interrupted");
        setPhase(QStringLiteral("mediaInterrupted"));
        m_status = QStringLiteral("检测到未恢复的编码包缺失，已 %1 秒没有新画面，正在请求关键帧…")
                       .arg(sinceFrame / 1000);
    } else if (sinceFrame >= WaitingAfterMs) {
        m_networkState = QStringLiteral("waiting");
        setPhase(QStringLiteral("waitingNewFrame"));
        m_status = QStringLiteral("等待新画面（最近画面 %1 秒前）。静态桌面不是故障。")
                       .arg(sinceFrame / 1000);
        if (sinceFrame >= InterruptedAfterMs) requestRemoteKeyFrame();
    } else {
        m_networkState = QStringLiteral("viewing");
        setPhase(QStringLiteral("viewing"));
        m_status = QStringLiteral("正在观看：%1").arg(label);
    }
}

void DeskShareController::mediaError(const media::MediaError &error)
{
    if (m_shuttingDown || m_stopping) return;
    if (error.code == QStringLiteral("backpressure")) {
        m_status = QStringLiteral("媒体背压：%1（已丢弃部分画面，等待关键帧）")
                       .arg(error.message.isEmpty() ? error.code : error.message);
        emit changed();
        return;
    }
    const auto message = QStringLiteral("媒体错误：%1")
                             .arg(error.message.isEmpty() ? error.code : error.message);
    if (m_publisherRoom || m_viewerRole)
        failRoom(message, QStringLiteral("媒体服务异常已结束本次会话；重新加载媒体插件后需重新主动发起。"), QStringLiteral("MEDIA_SERVICE_FAILED"));
    else {
        fail(message);
        setError(message, QStringLiteral("媒体服务异常；重新加载媒体插件后需重新主动发起。"));
    }
}

void DeskShareController::fail(const QString &message)
{
    if (m_shuttingDown || m_stopping) return;
    const bool wasPublishing = m_publisherRoom;
    const bool wasViewer = m_viewerRole;
    ++m_epoch;
    ++m_viewEpoch;
    ++m_lifecycle;
    m_acceptingFrames = false;
    clearOperation();
    m_state = QStringLiteral("failed");
    m_lastNetworkError = message;
    setError(message, QStringLiteral("重新选择来源并再次发起；连接仍有效时可重试。"));
    m_status = message;
    if (m_preview) m_preview->clear();
    detachActiveTracks();
    if (wasPublishing) {
        m_publisherRoom = false;
        clearRoomIdentity();
        if (m_network) {
            m_network->clearUpload();
            m_network->stopRoom(message);
        }
        m_networkState = m_connected ? QStringLiteral("connected") : QStringLiteral("disconnected");
    } else if (wasViewer) {
        m_viewerRole = false;
        clearRoomIdentity();
        if (m_network) {
            m_network->clearReceive();
            m_network->leaveRoom(message);
        }
        m_networkState = m_connected ? QStringLiteral("connected") : QStringLiteral("disconnected");
    }
    setPhase(m_connected ? QStringLiteral("failed") : QStringLiteral("disconnected"));
    emit changed();
}

void DeskShareController::failConnectionAttempt(const QString &message)
{
    if (m_shuttingDown) return;
    if (m_stopping) return;
    const bool hadRoomRole = m_publisherRoom || m_viewerRole;
    m_connected = false;
    m_sessionId = 0;
    m_udpPort = 0;
    m_pendingCreate = false;
    m_pendingJoin = false;
    m_publisherRoom = false;
    m_viewerRole = false;
    ++m_lifecycle;
    ++m_epoch;
    ++m_viewEpoch;
    m_acceptingFrames = false;
    clearRoomIdentity();
    if (hadRoomRole) {
        teardownSendTrack();
        teardownReceiveTrack();
    }
    m_networkState = QStringLiteral("failed");
    m_operation.clear();
    setError(message,
             QStringLiteral("核对地址、服务端插件与网络后点击“连接”重试；不会自动重连。"));
    m_status = message;
    m_lastNetworkError = message;
    if (m_preview) m_preview->clear();
    setPhase(QStringLiteral("disconnected"));
    emit changed();
}

void DeskShareController::onControlFailed(const QString &operation, int code,
                                          const QString &message)
{
    if (m_shuttingDown || m_stopping) return;
    const auto error = errorCodeName(ErrorCode(code));
    const auto detail = QStringLiteral("%1（%2）").arg(message, error);
    if (operation == QStringLiteral("hello")
        || (!m_connected && m_networkState == QStringLiteral("connecting"))) {
        failConnectionAttempt(QStringLiteral("连接服务端失败：%1").arg(detail));
        return;
    }
    if (operation == QStringLiteral("stream.configure") && m_publisherRoom) {
        failRoom(QStringLiteral("视频流配置失败：%1；已停止共享并关闭房间").arg(detail),
                 QStringLiteral("重新选择来源／档位后再次开始共享。"), QStringLiteral("STREAM_CONFIGURE_FAILED"));
        return;
    }
    if (operation == QStringLiteral("stream.ready") && m_viewerRole) {
        failRoom(QStringLiteral("接收确认失败：%1").arg(detail),
                 QStringLiteral("重新点击“加入观看”以重试。"), QStringLiteral("STREAM_READY_FAILED"));
        return;
    }
    if (operation == QStringLiteral("create") || operation == QStringLiteral("join")) {
        const bool create = operation == QStringLiteral("create");
        if (m_operation == QStringLiteral("create") || m_operation == QStringLiteral("join"))
            clearOperation();
        m_networkState = m_connected ? QStringLiteral("connected") : QStringLiteral("disconnected");
        setError(QStringLiteral("%1 失败：%2")
                     .arg(create ? QStringLiteral("创建房间") : QStringLiteral("加入房间"))
                     .arg(detail),
                 create ? QStringLiteral("检查服务端容量与地址后重试“开始共享”。")
                        : QStringLiteral("核对房号与服务端后重试“加入观看”。"));
        m_status = m_errorText;
        m_lastNetworkError = m_status;
        setPhase(m_connected ? (m_sourceValue.isEmpty() ? QStringLiteral("connected")
                                                        : QStringLiteral("ready"))
                             : QStringLiteral("disconnected"));
        emit changed();
        return;
    }
    if (operation == QStringLiteral("leave") || operation == QStringLiteral("stop")) {
        m_details = QStringLiteral("服务端清理响应：%1").arg(detail);
        emit changed();
        return;
    }
    if (operation == QStringLiteral("stream.feedback")
        || operation == QStringLiteral("stream.keyframe")) {
        m_details = QStringLiteral("服务端限制：%1").arg(detail);
        emit changed();
        return;
    }
    m_status = QStringLiteral("服务端返回错误：%1").arg(detail);
    setError(m_status, QString());
    emit changed();
}

void DeskShareController::tick()
{
    if (m_shuttingDown || m_stopping) return;
    bool updated = false;
    const auto now = nowMs();
    // Media-worker counters are diagnostic: they separate capture/encode drops
    // from network drops without changing any transfer decision. One async
    // request per second, ignored on failure.
    if (m_session && (capturing() || watching()) && now - m_lastMediaStatsMs >= 1000) {
        m_lastMediaStatsMs = now;
        m_session->statistics(this, [this](media::Result<media::Statistics> result) {
            if (!result.ok()) return;
            if (!m_sendId.isEmpty()) m_mediaSendStats = result.value.track(m_sendId).toVariantMap();
            if (!m_receiveId.isEmpty()) m_mediaReceiveStats = result.value.track(m_receiveId).toVariantMap();
        });
    }
    if (m_acceptingFrames && capturing() && m_sourceMissing) {
        if (!m_sourceMissingSinceMs) m_sourceMissingSinceMs = now;
        else if (now - m_sourceMissingSinceMs >= SourceMissingTimeoutMs) {
            failRoom(QStringLiteral("共享来源已关闭或不再可用；已停止共享，请重新选择来源。"),
                     QStringLiteral("刷新来源并重新选择后再开始共享。"), QStringLiteral("SOURCE_UNAVAILABLE"));
            return;
        }
    } else {
        m_sourceMissingSinceMs = 0;
    }
    if (m_acceptingFrames && capturing() && !m_capturedFrames && now - m_startRequestedMs >= 3000) {
        const auto message = QStringLiteral("等待来源首帧。部分静止窗口需要内容变化后才交付画面，可尝试滚动或切换到该窗口。");
        if (m_state != QStringLiteral("waiting") || m_status != message) {
            m_state = QStringLiteral("waiting");
            m_status = message;
            if (m_publisherRoom) setPhase(QStringLiteral("waitingSource"));
            updated = true;
        }
    }
    if (m_viewerRole && m_receiveReady) {
        if (m_decodedFrames == 0) requestRemoteKeyFrame();
        if (m_decodedFrames == 0 && now - m_viewStartMs >= FirstFrameTimeoutMs && !m_viewFailed) {
            m_viewFailed = true;
            failRoom(QStringLiteral("10 秒内没有收到可解码的首帧，已退出本次观看。"),
                     QStringLiteral("确认共享者仍在共享后重新加入。"), QStringLiteral("FIRST_FRAME_TIMEOUT"));
            return;
        }
        if (m_decodedFrames > 0) {
            updateNetworkStatus();
        }
        if (m_viewerRole) {
            // At most one report in flight. A missing acknowledgement inside the
            // control-request deadline re-arms reporting instead of silencing it.
            if (m_feedbackPending && now - m_feedbackSentMs >= FeedbackAckTimeoutMs) {
                m_feedbackPending = false;
                ++m_feedbackAckTimeouts;
            }
            const bool paceElapsed = m_feedbackAckMs == 0 || now - m_feedbackAckMs >= FeedbackMinIntervalMs;
            if (!m_feedbackPending && paceElapsed) {
                m_feedbackPending = true;
                m_feedbackSentMs = now;
                m_feedbackRequestEpochMs = QDateTime::currentMSecsSinceEpoch();
                if (m_feedbackAckMs > 0) {
                    m_feedbackLastAckToRequestMs = now - m_feedbackAckMs;
                    if (m_feedbackMinAckToRequestMs == 0
                        || m_feedbackLastAckToRequestMs < m_feedbackMinAckToRequestMs)
                        m_feedbackMinAckToRequestMs = m_feedbackLastAckToRequestMs;
                }
                ++m_feedbackRequests;
                const auto stats = m_network->stats();
                Feedback feedback;
                feedback.completePackets = stats.receiver.completePackets;
                feedback.missingFragments = stats.receiver.missingPackets;
                feedback.decodedFrames = quint64(m_decodedFrames);
                feedback.displayedFrames = quint64(m_decodedFrames);
                feedback.waiting = m_phase != QStringLiteral("viewing");
                feedback.receivedBytes = stats.inboundDelivered;
                m_network->sendFeedback(feedback, m_viewEpoch);
            }
        }
        updated = true;
    }
    if (m_acceptingFrames && capturing() && m_receiveReady && !m_viewerRole) {
        if (m_capturedFrames > 0 && m_decodedFrames == 0 && m_publisherRoom)
            setPhase(QStringLiteral("waitingFirstFrame"));
        if (m_decodedFrames > 0) {
            const auto idle = now - m_lastDecodedMs;
            QString state, status, phase;
            const bool interrupted = idle >= InterruptedAfterMs
                && m_lastPacketMs > m_lastDecodedMs + 100;
            if (interrupted) {
                state = QStringLiteral("interrupted");
                status = QStringLiteral("已 %1 秒没有新画面（静态桌面也可能如此），正在请求关键帧…")
                             .arg(idle / 1000);
                phase = m_publisherRoom ? QStringLiteral("mediaInterrupted")
                                        : QStringLiteral("previewing");
                requestLocalKeyFrame();
            } else if (idle >= WaitingAfterMs) {
                state = QStringLiteral("waiting");
                status = QStringLiteral("等待新画面（最近画面 %1 秒前）。静态桌面不是故障。")
                             .arg(idle / 1000);
                phase = m_publisherRoom ? QStringLiteral("waitingNewFrame")
                                        : QStringLiteral("previewing");
                if (idle >= InterruptedAfterMs) requestLocalKeyFrame();
            } else {
                state = QStringLiteral("live");
                status = liveStatus();
                phase = m_publisherRoom ? QStringLiteral("sharing")
                                        : QStringLiteral("previewing");
            }
            if (m_sourceMissing)
                status += QStringLiteral(" 所选来源暂不在可捕获列表，等待其恢复。");
            if (state != m_state || status != m_status) {
                m_state = state;
                m_status = status;
                updated = true;
            }
            setPhase(phase);
        }
    }
    if (capturing() || m_viewerRole) {
        if (m_publisherRoom || m_viewerRole) {
            const auto network = m_network->stats();
            const auto details =
                QStringLiteral("上行包 %1 · 上行报文 %2 · 接收包 %3 · 缺包 %4 · 已提交 %5 · 丢弃 %6")
                    .arg(network.uploadPackets)
                    .arg(network.datagramsSent)
                    .arg(network.receiver.completePackets)
                    .arg(network.receiver.missingPackets)
                    .arg(m_networkSubmitted)
                    .arg(m_dropped + m_uploadDropped);
            if (details != m_details) {
                m_details = details;
                updated = true;
            }
        } else {
            const auto details =
                (m_codec == QStringLiteral("bgra-tiles")
                     ? QStringLiteral("媒体包 %1 · 预览 %2 次 · 画面 %3×%4 · 丢弃 %5")
                     : QStringLiteral("编码 %1 帧 · 解码 %2 帧 · 画面 %3×%4 · 丢弃 %5"))
                    .arg(m_capturedFrames)
                    .arg(m_decodedFrames)
                    .arg(m_frameWidth)
                    .arg(m_frameHeight)
                    .arg(m_dropped);
            if (details != m_details) {
                m_details = details;
                updated = true;
            }
        }
    }
    if (updated) emit changed();
}

void DeskShareController::stopSharing()
{
    if (m_shuttingDown) return;
    if (busy()) return;
    if (m_publisherRoom || m_viewerRole || capturing()) {
        beginRoomStop(m_viewerRole, QStringLiteral("已停止共享。"), QString(), {});
        return;
    }
    if (hasPendingTracks()) {
        // R1/R3: the retry entry for a release that was not confirmed. It runs
        // through the same controlled stop task, so repeated clicks merge and
        // new room operations stay refused until the identity is confirmed.
        beginRoomStop(false, QStringLiteral("已停止共享。"), QString(), {});
        return;
    }
    if (m_tracksPending) {
        beginRoomStop(false, QStringLiteral("已停止共享。"), QString(), {});
        return;
    }
    m_status = QStringLiteral("当前没有进行中的共享。");
    emit changed();
}

void DeskShareController::stopCapture()
{
    if (!m_session || m_shuttingDown || busy()) return;
    if (!capturing()) return;
    beginRoomStop(false, QStringLiteral("已停止。"), QString(), {});
}

void DeskShareController::leaveRoom()
{
    if (m_shuttingDown || busy() || !m_viewerRole) return;
    beginRoomStop(true, QStringLiteral("已退出观看。"), QString(), {});
}

void DeskShareController::cancelOperation()
{
    if (!canCancel()) return;
    const auto operation = m_operation;
    ++m_lifecycle;
    ++m_epoch;
    ++m_viewEpoch;
    m_acceptingFrames = false;
    m_pendingCreate = false;
    m_pendingJoin = false;
    clearOperation();
    m_watchdog.stop();
    if (operation == QStringLiteral("connect")) {
        if (m_network) m_network->disconnectFromServer(QStringLiteral("已取消连接"));
        m_connected = false;
        m_networkState = QStringLiteral("disconnected");
        m_state = QStringLiteral("idle");
        clearError();
        m_status = QStringLiteral("已取消连接。未采集，也未建立房间。");
        setPhase(QStringLiteral("disconnected"));
        m_watchdog.start();
        emit changed();
        return;
    }
    if (operation == QStringLiteral("start")) {
        // The room already exists. Stop it before closing the control connection
        // so the server drops the room deterministically.
        beginRoomStop(false,
                      QStringLiteral("已取消启动共享；已关闭控制连接以清理服务端房间。"),
                      QString(), [this](bool ok) {
                          if (!ok) return;
                          if (m_network) m_network->disconnectFromServer(
                              QStringLiteral("已取消启动共享"));
                          m_connected = false;
                          m_networkState = QStringLiteral("disconnected");
                          setPhase(QStringLiteral("disconnected"));
                          emit changed();
                      });
        return;
    }
    // create/join in flight: closing the control connection is the only
    // deterministic cleanup. The server drops the room/member on TCP close.
    if (m_network) {
        m_network->clearUpload();
        m_network->clearReceive();
        m_network->disconnectFromServer(QStringLiteral("已取消操作"));
    }
    m_connected = false;
    m_networkState = QStringLiteral("disconnected");
    m_state = QStringLiteral("idle");
    clearRoomIdentity();
    clearError();
    m_status = QStringLiteral("已取消；已关闭控制连接以清理服务端房间／成员。可重新连接。");
    setPhase(QStringLiteral("disconnected"));
    m_watchdog.start();
    emit changed();
}

void DeskShareController::beginRoomStop(bool leave, const QString &status, const QString &error,
                                        std::function<void(bool)> completed)
{
    beginStopTask(leave, false, status, error, std::move(completed));
}

bool DeskShareController::refuseWhileReleasing()
{
    if (!hasPendingTracks() && !m_tracksPending) return false;
    if (hasPendingTracks()) {
        setError(QStringLiteral("媒体服务尚未确认释放上次的轨道（%1）。")
                     .arg(pendingReleaseSummary()),
                 QStringLiteral("等待释放或再次点击“停止共享”重试；确认回收前不会开始新的共享、观看或重配。"));
    } else {
        setError(QStringLiteral("上一次停止未完成，请先重试停止。"),
                 QStringLiteral("再次点击“停止共享”重试；确认回收前不会开始新的共享、观看或重配。"));
    }
    m_status = m_errorText;
    emit changed();
    return true;
}

QString DeskShareController::pendingReleaseSummary() const
{
    QStringList ids;
    for (const auto &entry : m_pendingReleases) {
        ids.append(QStringLiteral("%1:%2")
                       .arg(entry.send ? QStringLiteral("send") : QStringLiteral("receive"),
                            entry.id));
    }
    if (ids.isEmpty()) return QStringLiteral("无待回收轨道");
    return QStringLiteral("%1 条：%2").arg(ids.size()).arg(ids.join(QStringLiteral(", ")));
}

void DeskShareController::detachActiveTracks()
{
    const auto send = m_sendId;
    const auto receive = m_receiveId;
    m_sendId.clear();
    m_receiveId.clear();
    m_receiveDescriptor = {};
    m_receiveReady = false;
    m_receiveStarting = false;
    m_initializationData.clear();
    if (!send.isEmpty()) requestTrackRelease(send, true);
    if (!receive.isEmpty()) requestTrackRelease(receive, false);
}

void DeskShareController::requestTrackRelease(const QString &id, bool send)
{
    if (id.isEmpty() || !m_session) return;
    for (const auto &entry : m_pendingReleases) {
        if (entry.id == id) return;
    }
    PendingRelease entry;
    entry.id = id;
    entry.send = send;
    entry.attempt = ++m_releaseAttempt;
    entry.inFlight = true;
    m_pendingReleases.append(entry);
    issueTrackRelease(id, entry.attempt);
}

void DeskShareController::issueTrackRelease(const QString &id, quint64 attempt)
{
    if (!m_session) return;
    m_session->remove(id, this, [this, id, attempt](media::Result<bool> result) {
        onTrackReleased(id, attempt, result.ok(),
                        result.ok() ? QString() : result.error.message);
    });
}

void DeskShareController::onTrackReleased(const QString &id, quint64 attempt, bool ok,
                                          const QString &error)
{
    for (int index = 0; index < m_pendingReleases.size(); ++index) {
        auto &entry = m_pendingReleases[index];
        if (entry.id != id) continue;
        // A failure from an abandoned attempt says nothing about the newer
        // request that owns this identity now; a late success is still
        // authoritative because the worker cannot un-release a track.
        if (!ok && entry.attempt != attempt) return;
        if (ok) {
            entry.inFlight = false;
            m_pendingReleases.removeAt(index);
            m_lastReleaseError.clear();
        } else {
            entry.inFlight = false;
            ++m_releaseFailures;
            m_lastReleaseError =
                error.isEmpty() ? QStringLiteral("track.remove 返回失败") : error;
        }
        if (m_releaseDrain && !m_releaseDrain->finished) {
            if (!ok) completeReleaseDrain(false);
            else if (m_pendingReleases.isEmpty()) completeReleaseDrain(true);
        }
        if (m_pendingReleases.isEmpty()) {
            m_tracksPending = false;
            recoverAfterLateRelease();
        }
        emit changed();
        return;
    }
}

void DeskShareController::retryPendingReleases(std::function<void(bool)> completed)
{
    if (m_pendingReleases.isEmpty()) {
        m_tracksPending = false;
        if (completed) completed(true);
        return;
    }
    auto drain = std::make_shared<ReleaseDrain>();
    drain->done = std::move(completed);
    m_releaseDrain = drain;
    m_lastReleaseError.clear();
    // A removal already in flight is awaited, never duplicated. A retry only
    // issues a fresh request for identities whose attempt was abandoned.
    int index = 0;
    while (index < m_pendingReleases.size()) {
        if (m_pendingReleases[index].inFlight) {
            ++index;
            continue;
        }
        auto &entry = m_pendingReleases[index];
        entry.attempt = ++m_releaseAttempt;
        entry.inFlight = true;
        const auto id = entry.id;
        const auto attempt = entry.attempt;
        issueTrackRelease(id, attempt);
        if (index < m_pendingReleases.size() && m_pendingReleases[index].id == id) ++index;
    }
    QTimer::singleShot(StopTrackDrainMs, this, [this, drain] {
        if (drain->finished || m_releaseDrain.get() != drain.get()) return;
        // No answer within the bounded window. The attempts are abandoned (a
        // retry issues a fresh request) but every identity is kept.
        for (auto &entry : m_pendingReleases) entry.inFlight = false;
        completeReleaseDrain(false);
    });
}

void DeskShareController::completeReleaseDrain(bool ok)
{
    auto drain = m_releaseDrain;
    if (!drain || drain->finished) return;
    drain->finished = true;
    m_releaseDrain.reset();
    m_tracksPending = !ok || !m_pendingReleases.isEmpty();
    auto done = std::move(drain->done);
    if (done) done(ok);
}

void DeskShareController::recoverAfterLateRelease()
{
    if (m_shuttingDown || m_stopTask || !m_pendingReleases.isEmpty()) return;
    if (m_state != QStringLiteral("stop-failed")) return;
    // The worker eventually confirmed every release. The failure stayed visible
    // while it lasted; the entries are usable again only after this point.
    m_tracksPending = false;
    m_lastReleaseError.clear();
    m_state = QStringLiteral("idle");
    clearError();
    m_status = QStringLiteral("媒体轨道已确认释放，可以重新开始。");
    setPhase(m_connected ? (m_sourceValue.isEmpty() || m_sourceMissing
                                ? QStringLiteral("connected")
                                : QStringLiteral("ready"))
                         : QStringLiteral("disconnected"));
}

void DeskShareController::drainTracks(std::function<void(bool)> completed)
{
    detachActiveTracks();
    retryPendingReleases(std::move(completed));
}

bool DeskShareController::isStopTaskCurrent(const std::shared_ptr<StopTask> &task) const
{
    return task && m_stopTask && m_stopTask.get() == task.get()
        && task->generation == m_stopGeneration;
}

void DeskShareController::upgradeStopToUnload(const std::shared_ptr<StopTask> &task, int timeoutMs)
{
    if (!task) return;
    if (timeoutMs > 0) m_stopTimeoutMs = timeoutMs;
    if (task->unload) return;
    task->unload = true;
    m_stopUnload = true;
    if (m_unloading) return;
    m_unloading = true;
    m_shuttingDown = true;
    m_watchdog.stop();
    m_preferencesSaveTimer.stop();
    flushPreferences();
    m_acceptingFrames = false;
    ++m_lifecycle;
    ++m_epoch;
    ++m_viewEpoch;
}

std::shared_ptr<DeskShareController::StopTask>
DeskShareController::beginStopTask(bool leave, bool unload, const QString &status,
                                   const QString &error, std::function<void(bool)> completed)
{
    if (m_stopTask) {
        // R3: a repeated stop/unload request merges into the running task
        // instead of starting a second drain.
        auto task = m_stopTask;
        if (completed) task->waiters.append(std::move(completed));
        if (unload) upgradeStopToUnload(task, m_stopTimeoutMs);
        return task;
    }
    auto task = std::make_shared<StopTask>();
    task->generation = ++m_stopGeneration;
    task->leave = leave;
    task->status = status;
    task->error = error;
    task->unload = false;
    if (completed) task->waiters.append(std::move(completed));
    m_stopTask = task;
    if (unload) upgradeStopToUnload(task, m_stopTimeoutMs);
    const bool hadRoom = m_publisherRoom || m_viewerRole;
    const bool wasViewer = m_viewerRole;
    task->hadRoom = hadRoom;
    m_stopUnload = task->unload;
    m_stopping = true;
    m_operation = QStringLiteral("stop");
    m_acceptingFrames = false;
    ++m_epoch;
    ++m_viewEpoch;
    ++m_lifecycle;
    m_watchdog.stop();
    m_publisherRoom = false;
    m_viewerRole = false;
    m_configurePending = false;
    m_awaitingKeyFrame = false;
    m_receiveReady = false;
    m_receiveStarting = false;
    clearRoomIdentity();
    if (m_network) {
        m_network->clearUpload();
        m_network->clearReceive();
        if (hadRoom) {
            if (leave || wasViewer) m_network->leaveRoom(status);
            else m_network->stopRoom(status);
        }
    }
    if (m_preview) m_preview->clear();
    m_networkState = m_connected ? QStringLiteral("connected") : QStringLiteral("disconnected");
    setPhase(QStringLiteral("stopping"));
    m_status = task->unload ? QStringLiteral("正在停止共享、观看并排空网络与媒体资源…")
                            : QStringLiteral("正在停止采集并释放媒体资源…");
    emit changed();
    if (task->unload) {
        // Bounded chance to write stop/leave before the sockets are aborted.
        // No room means no grace delay at all.
        const int grace = hadRoom ? StopGraceMs : 0;
        QTimer::singleShot(grace, this, [this, task] {
            if (!isStopTaskCurrent(task)) return;
            startStopNetworkDrain(task);
        });
    } else {
        startStopMediaDrain(task);
    }
    return task;
}

void DeskShareController::startStopMediaDrain(const std::shared_ptr<StopTask> &task)
{
    drainTracks([this, task](bool ok) {
        if (!isStopTaskCurrent(task)) return;
        if (!ok) {
            finishStopTask(task, false);
            return;
        }
        task->mediaDrained = true;
        m_mediaDrained = m_pendingReleases.isEmpty();
        if (task->unload && !m_networkDrained) {
            startStopNetworkDrain(task);
            return;
        }
        finishStopTask(task, true);
    });
}

void DeskShareController::startStopNetworkDrain(const std::shared_ptr<StopTask> &task)
{
    if (!isStopTaskCurrent(task)) return;
    if (m_networkDrained) {
        task->networkDrained = true;
        if (!task->mediaDrained) {
            startStopMediaDrain(task);
            return;
        }
        finishStopTask(task, true);
        return;
    }
    if (!m_network) {
        m_networkDrained = true;
        task->networkDrained = true;
        if (!task->mediaDrained) {
            startStopMediaDrain(task);
            return;
        }
        finishStopTask(task, true);
        return;
    }
    m_network->shutdownAsync(
        [this, task](bool ok) {
            if (!isStopTaskCurrent(task)) return;
            if (!ok) {
                finishStopTask(task, false);
                return;
            }
            m_networkDrained = true;
            task->networkDrained = true;
            if (!task->mediaDrained) {
                startStopMediaDrain(task);
                return;
            }
            finishStopTask(task, true);
        },
        m_stopTimeoutMs);
}

void DeskShareController::finishStopTask(const std::shared_ptr<StopTask> &task, bool ok)
{
    if (!isStopTaskCurrent(task)) return;
    m_stopTask.reset();
    m_stopping = false;
    m_operation.clear();
    m_tracksPending = !m_pendingReleases.isEmpty();
    m_mediaDrained = task->mediaDrained && m_pendingReleases.isEmpty();
    m_networkDrained = m_networkDrained || task->networkDrained;
    if (ok) {
        m_tracksPending = false;
        m_state = task->unload ? QStringLiteral("stopped") : QStringLiteral("idle");
        m_status = task->status.isEmpty() ? QStringLiteral("已停止。") : task->status;
        m_details = QStringLiteral("已停止 · 最后编码 %1 帧 · 解码 %2 帧 · 丢弃 %3 次")
                        .arg(m_capturedFrames)
                        .arg(m_decodedFrames)
                        .arg(m_dropped);
        m_lastPacketMs = 0;
        m_lastDecodedMs = 0;
        m_frameWidth = 0;
        m_frameHeight = 0;
        if (m_preview) m_preview->clear();
        if (!m_publisherRoom && !m_viewerRole && m_connected && !m_preferencesError.isEmpty())
            m_details = m_preferencesError;
        setPhase(QStringLiteral("ended"));
    } else {
        m_tracksPending = true;
        m_state = QStringLiteral("stop-failed");
        QString message;
        if (task->unload && !task->networkDrained && !m_networkDrained) {
            message = QStringLiteral("网络排空超时；DLL、控制器与媒体租约已保留。");
        } else if (!m_lastReleaseError.isEmpty()) {
            message = QStringLiteral("媒体轨道释放失败：%1%2")
                          .arg(m_lastReleaseError,
                               task->unload ? QStringLiteral("；DLL、控制器与媒体租约已保留。")
                                            : QString());
        } else if (!m_pendingReleases.isEmpty()) {
            message = task->unload
                          ? QStringLiteral("媒体轨道释放未在期限内确认；DLL、控制器与媒体租约已保留。")
                          : QStringLiteral("媒体轨道释放未在期限内确认；已保留待回收的轨道。");
        } else {
            message = task->error.isEmpty() ? QStringLiteral("停止未完成。") : task->error;
        }
        setError(message,
                 task->unload
                     ? QStringLiteral("再次请求退出以重试停止。")
                     : QStringLiteral(
                           "再次点击“停止共享”重试；确认回收前不会开始新的共享、观看或重配。"));
        m_status = m_errorText;
        setPhase(QStringLiteral("stopFailed"));
    }
    m_watchdog.start();
    const auto waiters = task->waiters;
    task->waiters.clear();
    for (const auto &waiter : waiters) waiter(ok);
    emit changed();
}

void DeskShareController::abandonStopTask(const QString &reason)
{
    if (!m_stopTask) return;
    auto task = m_stopTask;
    m_stopTask.reset();
    ++m_stopGeneration;
    m_stopping = false;
    m_operation.clear();
    m_tracksPending = !m_pendingReleases.isEmpty();
    m_stopUnload = false;
    if (!reason.isEmpty()) m_details = reason;
    const auto waiters = task->waiters;
    task->waiters.clear();
    for (const auto &waiter : waiters) waiter(false);
}

void DeskShareController::shutdownAsync(std::function<void(bool)> completed, int timeoutMs)
{
    if (m_state == QStringLiteral("stopped") && !m_stopTask && !hasPendingTracks()) {
        completed(true);
        return;
    }
    if (timeoutMs > 0) m_stopTimeoutMs = timeoutMs;
    if (m_stopTask) {
        auto task = m_stopTask;
        upgradeStopToUnload(task, timeoutMs);
        if (completed) task->waiters.append(std::move(completed));
        return;
    }
    beginStopTask(false, true, QStringLiteral("已停止并排空网络与媒体资源。"), QString(),
                  std::move(completed));
}

bool DeskShareController::shutdown(int timeoutMs)
{
    if (m_state == QStringLiteral("stopped") && !hasPendingTracks()) return true;
    bool result = false;
    bool done = false;
    shutdownAsync(
        [&result, &done](bool ok) {
            result = ok;
            done = true;
        },
        timeoutMs);
    // The blocking entry exists only for the emergency destructor path; the
    // normal host path uses shutdownAsync. It waits with a real event loop so
    // queued socket/thread completions are delivered.
    if (!done) {
        QElapsedTimer timer;
        timer.start();
        while (!done && timer.elapsed() < timeoutMs + 1000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        if (!done) {
            if (m_network) {
                const bool stopped = m_network->shutdown(0);
                if (!stopped) return false;
                m_networkDrained = true;
            }
            const auto send = m_sendId, receive = m_receiveId;
            clearTrackState();
            if (m_session && !send.isEmpty()) m_session->remove(send, this);
            if (m_session && !receive.isEmpty()) m_session->remove(receive, this);
            if (m_session) {
                for (const auto &pending : m_pendingReleases) m_session->remove(pending.id, this);
            }
            m_pendingReleases.clear();
            m_tracksPending = false;
            m_state = QStringLiteral("stopped");
            setPhase(QStringLiteral("ended"));
            return true;
        }
    }
    return result;
}

QVariantMap DeskShareController::statistics() const
{
    QVariantMap map;
    const auto network = m_network ? m_network->stats() : NetworkStats{};
    map.insert(QStringLiteral("connected"), m_connected);
    map.insert(QStringLiteral("networkState"), m_networkState);
    map.insert(QStringLiteral("publisher"), m_publisherRoom);
    map.insert(QStringLiteral("viewer"), m_viewerRole);
    map.insert(QStringLiteral("roomActive"), m_roomId != 0);
    map.insert(QStringLiteral("publishEpoch"), QString::number(m_publishEpoch));
    map.insert(QStringLiteral("localAddress"), network.localAddress);
    map.insert(QStringLiteral("localUdpPort"), network.localUdpPort);
    map.insert(QStringLiteral("serverUdpPort"), network.serverUdpPort);
    map.insert(QStringLiteral("mediaStreamEpoch"), QString::number(network.mediaStreamEpoch));
    map.insert(QStringLiteral("viewEpoch"), QString::number(m_viewEpoch));
    map.insert(QStringLiteral("viewerCount"), m_viewerCount);
    map.insert(QStringLiteral("capturedFrames"), m_capturedFrames);
    map.insert(QStringLiteral("decodedFrames"), m_decodedFrames);
    map.insert(QStringLiteral("networkSubmitted"), QString::number(m_networkSubmitted));
    map.insert(QStringLiteral("uploadDropped"), QString::number(m_uploadDropped));
    map.insert(QStringLiteral("gaps"), QString::number(m_gapDetected));
    map.insert(QStringLiteral("dropped"), QString::number(m_dropped));
    map.insert(QStringLiteral("uplink"), m_network ? m_network->uplinkDescription() : QString());
    map.insert(QStringLiteral("tcpRequestsSent"), QString::number(network.tcpRequestsSent));
    map.insert(QStringLiteral("tcpResponsesReceived"), QString::number(network.tcpResponsesReceived));
    map.insert(QStringLiteral("controlErrors"), QString::number(network.controlErrors));
    map.insert(QStringLiteral("lateResponses"), QString::number(network.lateResponses));
    map.insert(QStringLiteral("datagramsSent"), QString::number(network.datagramsSent));
    map.insert(QStringLiteral("datagramsDropped"), QString::number(network.datagramsDropped));
    map.insert(QStringLiteral("uploadPackets"), QString::number(network.uploadPackets));
    map.insert(QStringLiteral("uploadDroppedBudget"), QString::number(network.uploadPacketsDroppedBudget));
    map.insert(QStringLiteral("uploadDroppedAge"), QString::number(network.uploadPacketsDroppedAge));
    map.insert(QStringLiteral("uploadDroppedQueue"), QString::number(network.uploadPacketsDroppedQueue));
    map.insert(QStringLiteral("uploadDroppedPacing"), QString::number(network.uploadPacketsDroppedPacing));
    map.insert(QStringLiteral("uploadDroppedDeadline"), QString::number(network.uploadPacketsDroppedDeadline));
    map.insert(QStringLiteral("uploadDroppedDependency"), QString::number(network.uploadPacketsDroppedDependency));
    map.insert(QStringLiteral("uploadDeferred"), QString::number(network.uploadPacketsDeferred));
    map.insert(QStringLiteral("pacingWaits"), QString::number(network.pacingWaits));
    map.insert(QStringLiteral("publishRateBytesPerSecond"),
               QString::number(network.publishRateBytesPerSecond));
    map.insert(QStringLiteral("publishBudgetBytes"), QString::number(network.publishBudgetBytes));
    map.insert(QStringLiteral("guiPendingNotices"), network.guiPendingNotices);
    map.insert(QStringLiteral("guiPeakNotices"), network.guiPeakNotices);
    map.insert(QStringLiteral("guiNoticesCoalesced"), QString::number(network.guiNoticesCoalesced));
    map.insert(QStringLiteral("guiNoticesDelivered"), QString::number(network.guiNoticesDelivered));
    map.insert(QStringLiteral("inboundDatagrams"), QString::number(network.inboundDatagrams));
    map.insert(QStringLiteral("inboundDelivered"), QString::number(network.inboundDelivered));
    map.insert(QStringLiteral("inboundRejected"), QString::number(network.inboundRejected));
    map.insert(QStringLiteral("inboundDroppedBudget"), QString::number(network.inboundDroppedBudget));
    map.insert(QStringLiteral("udpRegisters"), QString::number(network.udpRegistersSent));
    map.insert(QStringLiteral("udpAcks"), QString::number(network.udpAckReceived));
    map.insert(QStringLiteral("keepalives"), QString::number(network.keepalivesSent));
    map.insert(QStringLiteral("completePackets"), QString::number(network.receiver.completePackets));
    map.insert(QStringLiteral("submittedPackets"), QString::number(network.receiver.submittedPackets));
    map.insert(QStringLiteral("missingPackets"), QString::number(network.receiver.missingPackets));
    map.insert(QStringLiteral("gapsDetected"), QString::number(network.receiver.gaps));
    map.insert(QStringLiteral("droppedUntilKeyFrame"),
               QString::number(network.receiver.droppedUntilKeyFrame));
    map.insert(QStringLiteral("duplicateDatagrams"), QString::number(network.receiver.duplicateDatagrams));
    map.insert(QStringLiteral("rejectedDatagrams"), QString::number(network.receiver.rejectedDatagrams));
    map.insert(QStringLiteral("expiredFragments"), QString::number(network.receiver.expiredFragments));
    map.insert(QStringLiteral("pendingPackets"), network.receiver.pendingPackets);
    map.insert(QStringLiteral("bufferedBytes"), network.receiver.bufferedBytes);
    map.insert(QStringLiteral("heldPackets"), network.receiver.heldPackets);
    map.insert(QStringLiteral("heldBytes"), QString::number(network.receiver.heldBytes));
    map.insert(QStringLiteral("peakPendingPackets"), network.receiver.peakPendingPackets);
    map.insert(QStringLiteral("peakBufferedBytes"), network.receiver.peakBufferedBytes);
    map.insert(QStringLiteral("peakHeldPackets"), network.receiver.peakHeldPackets);
    map.insert(QStringLiteral("peakHeldBytes"), QString::number(network.receiver.peakHeldBytes));
    map.insert(QStringLiteral("outboundQueued"), network.outboundQueued);
    map.insert(QStringLiteral("outboundBytes"), network.outboundBytes);
    map.insert(QStringLiteral("outboundPeakBytes"), QString::number(network.outboundPeakBytes));
    map.insert(QStringLiteral("outboundPeakPackets"), network.outboundPeakPackets);
    map.insert(QStringLiteral("inboundQueued"), network.inboundQueued);
    map.insert(QStringLiteral("inboundBytes"), QString::number(network.inboundBytes));
    map.insert(QStringLiteral("inboundPeakBytes"), QString::number(network.inboundPeakBytes));
    map.insert(QStringLiteral("inboundPeakPackets"), network.inboundPeakPackets);
    map.insert(QStringLiteral("lastError"), m_lastNetworkError);
    // Media-worker track counters (one-second cached snapshot). They are
    // diagnostic only and never drive transfer decisions.
    const auto addMediaStats = [&map](const QString &prefix, const QVariantMap &track) {
        for (auto it = track.cbegin(); it != track.cend(); ++it)
            if (it.key().startsWith(QStringLiteral("tile"))) map.insert(prefix + it.key(), it.value());
        for (const auto &key : {QStringLiteral("captured"), QStringLiteral("dropped"),
                                QStringLiteral("frames"), QStringLiteral("received"),
                                QStringLiteral("processingUs"), QStringLiteral("peakProcessingUs"),
                                QStringLiteral("decodedOutput"), QStringLiteral("decodedQueueDrops"),
                                QStringLiteral("decodedQueueSize"), QStringLiteral("decodedQueuePeak"),
                                QStringLiteral("previewPublished"), QStringLiteral("previewOverwritten"),
                                QStringLiteral("keyFrameRefreshes"), QStringLiteral("tickCount"),
                                QStringLiteral("peakTickGapUs")}) {
            if (track.contains(key)) map.insert(prefix + key, track.value(key));
        }
    };
    addMediaStats(QStringLiteral("mediaSend"), m_mediaSendStats);
    addMediaStats(QStringLiteral("mediaReceive"), m_mediaReceiveStats);
    return map;
}

QVariantMap DeskShareController::recoveryObservation() const
{
    const bool terminal = !m_viewerRole && !m_stopping
        && (m_phase == QStringLiteral("failed") || m_phase == QStringLiteral("disconnected"))
        && m_viewerFailure.contains(QStringLiteral("visibleEpochMs"));
    return {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("stream"), m_observedStream},
            {QStringLiteral("state"), terminal ? QStringLiteral("terminal")
                : m_stopping ? QStringLiteral("terminating")
                : m_viewerRole ? (m_receiveReady ? QStringLiteral("active") : QStringLiteral("preparing"))
                : QStringLiteral("idle")},
            {QStringLiteral("failure"), m_viewerFailure}};
}

QVariantMap DeskShareController::feedbackObservation() const
{
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("viewer"), m_viewerRole},
            {QStringLiteral("receiveReady"), m_receiveReady},
            {QStringLiteral("pending"), m_feedbackPending},
            {QStringLiteral("requests"), QString::number(m_feedbackRequests)},
            {QStringLiteral("responses"), QString::number(m_feedbackResponses)},
            {QStringLiteral("rateLimited"), QString::number(m_feedbackRateLimited)},
            {QStringLiteral("ackTimeouts"), QString::number(m_feedbackAckTimeouts)},
            {QStringLiteral("minIntervalMs"), double(FeedbackMinIntervalMs)},
            {QStringLiteral("lastRequestEpochMs"), double(m_feedbackRequestEpochMs)},
            {QStringLiteral("lastResponseEpochMs"), double(m_feedbackResponseEpochMs)},
            {QStringLiteral("lastRoundTripMs"), double(m_feedbackLastRoundTripMs)},
            {QStringLiteral("lastAckToRequestMs"), double(m_feedbackLastAckToRequestMs)},
            {QStringLiteral("minAckToRequestMs"), double(m_feedbackMinAckToRequestMs)}};
}

QVariantMap DeskShareController::lifecycleStatistics() const
{
    QVariantMap map;
    map.insert(QStringLiteral("phase"), m_phase);
    map.insert(QStringLiteral("phaseLabel"), m_phaseLabel);
    map.insert(QStringLiteral("operation"), m_operation);
    map.insert(QStringLiteral("shuttingDown"), m_shuttingDown);
    map.insert(QStringLiteral("stopping"), m_stopping);
    map.insert(QStringLiteral("unloading"), m_stopUnload);
    map.insert(QStringLiteral("networkDrained"), m_networkDrained);
    map.insert(QStringLiteral("mediaDrained"), m_mediaDrained);
    map.insert(QStringLiteral("tracksPending"), m_tracksPending);
    map.insert(QStringLiteral("releasingTracks"), m_pendingReleases.size());
    map.insert(QStringLiteral("pendingReleaseSummary"), pendingReleaseSummary());
    map.insert(QStringLiteral("releaseFailures"), QString::number(m_releaseFailures));
    map.insert(QStringLiteral("lastReleaseError"), m_lastReleaseError);
    map.insert(QStringLiteral("stopGeneration"), QString::number(m_stopGeneration));
    map.insert(QStringLiteral("stopTaskActive"), m_stopTask != nullptr);
    map.insert(QStringLiteral("acceptingFrames"), m_acceptingFrames);
    map.insert(QStringLiteral("lifecycle"), QString::number(m_lifecycle));
    map.insert(QStringLiteral("epoch"), QString::number(m_epoch));
    map.insert(QStringLiteral("viewEpoch"), QString::number(m_viewEpoch));
    map.insert(QStringLiteral("sourceEpoch"), QString::number(m_sourceEpoch));
    map.insert(QStringLiteral("sendTrack"), m_sendId);
    map.insert(QStringLiteral("receiveTrack"), m_receiveId);
    map.insert(QStringLiteral("sessionOpen"), m_sessionOpen);
    map.insert(QStringLiteral("preferencesError"), m_preferencesError);
    map.insert(QStringLiteral("errorText"), m_errorText);
    map.insert(QStringLiteral("nextAction"), m_nextAction);
    map.insert(QStringLiteral("canConnect"), canConnect());
    map.insert(QStringLiteral("canDisconnect"), canDisconnect());
    map.insert(QStringLiteral("canStartShare"), canStartShare());
    map.insert(QStringLiteral("canStopShare"), canStopShare());
    map.insert(QStringLiteral("canJoin"), canJoin());
    map.insert(QStringLiteral("canLeave"), canLeave());
    map.insert(QStringLiteral("canCancel"), canCancel());
    return map;
}

} // namespace vusiif::zto::desktop_share
