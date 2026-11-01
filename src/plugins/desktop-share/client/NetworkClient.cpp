#include "NetworkClient.h"
#include "MediaEndpointActivity.h"

#include <desktop-share/PendingRequests.h>
#include <desktop-share/ByteRateLimiter.h>
#include <desktop-share/PublishQueue.h>
#include <desktop-share/SendReservation.h>
#include <desktop-share/VideoMailbox.h>

#include <ztprotocol/WireProtocol.h>

#include <QAbstractSocket>
#include <QNetworkProxy>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QNetworkDatagram>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QDeadlineTimer>
#include <QUdpSocket>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>

namespace vusiif::zto::desktop_share {

namespace {

qint64 nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

qint64 nowMs() { return nowUs() / 1000; }

// Transport diagnostics are opt-in: they name operations and counters only,
// never tokens, room codes, control payloads or video content.
bool diagnostics()
{
    static const bool enabled =
        qEnvironmentVariableIntValue("ZTO_DESKTOP_SHARE_DIAGNOSTICS") != 0;
    return enabled;
}

// Connection and business handshake share the fixed five-second deadline: a
// TCP accept alone is not a usable desktop-share connection.
constexpr qint64 ConnectTimeoutUs = 5'000'000;
constexpr char BuiltinRoute[] = "builtin";
constexpr qint64 RegisterRetryUs = 500'000;
constexpr int MaxRegisterAttempts = 10;
constexpr qint64 KeepaliveUs = 5'000'000;
constexpr qint64 UdpIdleUs = 15'000'000;
constexpr int ReceiveQueuePackets = 4;
constexpr qint64 ReceiveQueueBytes = 4 * 1024 * 1024;

// The receiver expires an unfinished assembly 200 ms after its first fragment
// (StreamReceiver::GapTimeoutUs / PacketReassembler). The publisher must finish
// the *last* fragment inside that window, so it may not start a packet whose
// transmission would cross it, and it must stop a started packet that stalls.
constexpr qint64 ReassemblyDeadlineUs = 200'000;
// Margin kept for the tail fragments to travel and be processed by the viewer.
constexpr qint64 SendCompletionMarginUs = 50'000;
constexpr qint64 ActiveSendDeadlineUs = ReassemblyDeadlineUs - SendCompletionMarginUs;
// Extra slack on top of the bucket refill time when bounding an admission wait.
constexpr qint64 AdmissionSlackUs = 250'000;
constexpr int UploadFragmentsPerTurn = 16;
// Upper bound for the adaptive catch-up batch: a turn may write more fragments
// when the event loop is slow, but never an unbounded slice of the packet.
constexpr int UploadFragmentsPerTurnMax = 128;
constexpr int UploadTurnMs = 1;
constexpr int UploadRetryMaxMs = 50;
// A turn that takes longer than this is worth a diagnostic line: the whole
// packet must still finish inside the active-send deadline.
constexpr qint64 UploadSlowTurnUs = 5'000;

} // namespace

// Fixed set of GUI notification kinds. The network thread merges every
// notification into its own slot and posts at most one queued wakeup, so a GUI
// thread that is blocked for a long time keeps a constant pending-task count
// instead of one posted callback per lost packet. Each slot keeps the newest
// payload of its kind; counts accumulate.
enum class GuiNotice : quint8 {
    Connected = 0,
    ConnectFailed,
    ConnectionLost,
    ControlFailed,
    RoomCreated,
    RoomJoined,
    RoomStateChanged,
    StreamConfig,
    StreamKeyframe,
    RoomClosed,
    StreamConfigured,
    StreamReadyConfirmed,
    UdpReady,
    UploadDropped,
    StreamGap,
    FeedbackCompleted,
    Count,
};

// Why a queued/active upload packet was discarded. Kept as a fixed enum so the
// merged notification stays bounded and the cumulative counts are preserved.
enum class UploadDropCause : quint8 {
    NotPublishable = 0,
    Fragment,
    Pacing,
    AdmissionTimeout,
    AdmissionAge,
    SendDeadline,
    Encode,
    Socket,
    QueueAge,
    Dependency,
    Count,
};

QString uploadDropCauseName(UploadDropCause cause)
{
    switch (cause) {
    case UploadDropCause::NotPublishable: return QStringLiteral("当前流不可发布");
    case UploadDropCause::Fragment: return QStringLiteral("编码包无法分片");
    case UploadDropCause::Pacing: return QStringLiteral("超出发送突发上限");
    case UploadDropCause::AdmissionTimeout: return QStringLiteral("等待发送预算超时");
    case UploadDropCause::AdmissionAge: return QStringLiteral("等待预算期间超出发送年龄");
    case UploadDropCause::SendDeadline: return QStringLiteral("超出重组发送期限");
    case UploadDropCause::Encode: return QStringLiteral("UDP 编码失败");
    case UploadDropCause::Socket: return QStringLiteral("UDP 发送失败");
    case UploadDropCause::QueueAge: return QStringLiteral("排队超时");
    case UploadDropCause::Dependency: return QStringLiteral("依赖失效");
    case UploadDropCause::Count: break;
    }
    return QStringLiteral("未知原因");
}

struct PendingControlFailure {
    QString operation;
    int code = 0;
    QString message;
};

struct GuiNoticeSlot {
    bool pending = false;
    quint64 sequence = 0;
    quint64 count = 0;
    quint64 first = 0;
    quint64 second = 0;
    qint64 value = 0;
    int code = 0;
    bool flag = false;
    // UdpReady only: a failure that still has to reach the controller, kept apart
    // from the current registration state so a later success cannot hide it. The
    // drain delivers the failure first and the recovered state after it.
    bool failurePending = false;
    // Room/stream/media lifetime the notice payload describes. A lifecycle-scoped
    // notice whose generation no longer matches the current one is stale and is
    // never delivered into a newer session, neither from the board nor from a
    // list a drain already copied.
    quint64 generation = 0;
    QString text;
    QString text2;
    RoomInfo room;
    StreamFormat format;
    // Only GuiNotice::ControlFailed uses this list; the number of distinct
    // operation names is a closed set and the hard cap keeps it bounded.
    QList<PendingControlFailure> failures;
};

constexpr int GuiNoticeCount = int(GuiNotice::Count);
constexpr int UploadDropCauseCount = int(UploadDropCause::Count);
constexpr int MaxPendingControlFailures = 16;

// Notices whose payload describes one room, stream or media-endpoint lifetime.
// They are only valid while that lifetime is the current one. Connection-level
// notices (and the aggregated upload-drop counters) stay valid across a room
// change, so a callback that opens or closes a room can never hide a lost
// control connection or a control error from the controller.
bool noticeDescribesLifetime(GuiNotice kind)
{
    switch (kind) {
    case GuiNotice::Connected:
    case GuiNotice::ConnectFailed:
    case GuiNotice::ConnectionLost:
    case GuiNotice::ControlFailed:
    case GuiNotice::UploadDropped:
    case GuiNotice::Count:
        return false;
    case GuiNotice::RoomCreated:
    case GuiNotice::RoomJoined:
    case GuiNotice::RoomStateChanged:
    case GuiNotice::StreamConfig:
    case GuiNotice::StreamKeyframe:
    case GuiNotice::RoomClosed:
    case GuiNotice::StreamConfigured:
    case GuiNotice::StreamReadyConfirmed:
    case GuiNotice::UdpReady:
    case GuiNotice::StreamGap:
    case GuiNotice::FeedbackCompleted:
        break;
    }
    return true;
}

// Independent mutex: the board is posted to while the data-plane mutex is held,
// and read by the GUI thread, so the two locks must never nest in one order.
struct GuiNoticeBoard {
    QMutex mutex;
    GuiNoticeSlot entries[GuiNoticeCount];
    quint64 dropCauses[UploadDropCauseCount] = {};
    quint64 sequence = 0;
    int pending = 0;
    int peak = 0;
    quint64 coalesced = 0;
    quint64 delivered = 0;
    // Notices dropped because they belonged to a room or stream that is gone.
    quint64 stale = 0;
    // Generation of the current room/media lifetime. It advances whenever a room
    // is adopted or closed on the network thread and whenever the GUI thread
    // issues a lifecycle command (stop, leave, create, join, connect, disconnect,
    // stop of the client), so a notice copied before that point can never be
    // delivered as the state of the session that exists afterwards.
    quint64 udpGeneration = 0;
    std::atomic<bool> wake{false};
};

// Requires `GuiNoticeBoard::mutex` held. Starts a new room/media lifetime: the
// epoch advances, queued lifecycle notices of the old lifetime are discarded and
// the UDP readiness slot (whose payload carries both the unconsumed failure and
// the current registration state) is reset. Callers must not hold any other lock
// and must never be a signal callback itself.
void advanceNoticeLifetimeLocked(GuiNoticeBoard &board)
{
    ++board.udpGeneration;
    for (int index = 0; index < GuiNoticeCount; ++index) {
        auto &slot = board.entries[index];
        if (!slot.pending) continue;
        if (!noticeDescribesLifetime(GuiNotice(index))) continue;
        slot.pending = false;
        --board.pending;
        ++board.stale;
    }
    auto &udp = board.entries[int(GuiNotice::UdpReady)];
    udp.generation = board.udpGeneration;
    udp.failurePending = false;
    udp.flag = false;
    udp.text.clear();
}

// Shared between the GUI facade and the network thread. Every access is
// guarded; the wakeup flags collapse any number of pushes into one queued
// wakeup per direction.
struct DataPlane {
    QMutex mutex;
    PublishQueue outbound;
    VideoMailbox inbound{ReceiveQueuePackets, ReceiveQueueBytes, 0};
    GuiNoticeBoard notices;
    qint64 outboundPeakBytes = 0;
    int outboundPeakPackets = 0;
    qint64 inboundPeakBytes = 0;
    int inboundPeakPackets = 0;
    quint64 uploadQueueRejected = 0;
    bool closed = false;
    std::atomic<bool> wakeOutbound{false};
    std::atomic<bool> wakeInbound{false};
    NetworkStats snapshot;
};

// Owned by the dedicated network thread. It never blocks on the GUI thread and
// never touches media sessions.
class NetworkEngine final : public QObject {
public:
    NetworkEngine(std::shared_ptr<DataPlane> plane, NetworkClient *facade)
        : m_plane(std::move(plane)), m_facade(facade)
    {
    }

    void begin();
    void stop();
    void connectTo(const QString &address, quint16 port);
    void disconnectFrom(const QString &reason);
    void create(const QString &profile);
    void join(const QString &code);
    void leave(const QString &reason);
    void stopRoom(const QString &reason);
    void configure(const StreamFormat &format);
    void ready(quint64 epoch);
    void keyframe(quint64 epoch);
    void feedback(const Feedback &values, quint64 epoch);
    void beginReceive(quint64 epoch);
    void resyncReceive();
    void clearReceive();
    void clearUpload();
    void drainOutbound();
    void beginActive(VideoPacket packet, qint64 enqueuedUs);
    void retryAdmission();
    void startSending();
    void onUploadTick();
    void sendBurst();
    int fragmentBatch(qint64 now) const;
    void finishActivePacket();
    void finishActiveAborted(const QString &reason, UploadDropCause cause);
    void reportQueueDropsLocked();
    void releaseReservation();
    bool newerKeyFrameWaiting();

private:
    void tcpConnected();
    void tcpReadyRead();
    void lost(const QString &reason);
    void failConnect(const QString &message);
    void handleBuiltin(const QByteArray &payload);
    bool handleControl(const QByteArray &payload);
    void handleResponse(const Response &response, Operation operation);
    void handleEvent(const Event &event);
    void adoptRoom(const RoomInfo &room);
    void clearRoom();
    void startRegistration();
    void sendRegister(bool keepalive);
    void handleAck();
    void udpReadyRead();
    void deliverInbound(VideoPacket packet);
    void tick();
    void publishStats();
    quint32 sendRequest(Operation operation, const std::function<void(Request &)> &fill);
    void reportControlFailure(const QString &operation, ErrorCode code, const QString &message);

    // Merges one notification into the fixed GUI board and posts at most one
    // queued wakeup. `fill` runs with the board mutex held and must only update
    // the slot payload (newest state wins); counts are accumulated here.
    template <class F> void notifyGui(GuiNotice kind, F fill)
    {
        NetworkClient *facade = m_facade;
        if (!facade || !m_plane) return;
        bool wake = false;
        {
            QMutexLocker lock(&m_plane->notices.mutex);
            auto &board = m_plane->notices;
            auto &slot = board.entries[int(kind)];
            if (!slot.pending) {
                slot.pending = true;
                slot.sequence = ++board.sequence;
                // Stamped with the lifetime the payload was produced in; the
                // drain compares it with the live one before it emits.
                slot.generation = board.udpGeneration;
                ++board.pending;
                board.peak = std::max(board.peak, board.pending);
            } else {
                ++board.coalesced;
            }
            ++slot.count;
            fill(slot);
            wake = !board.wake.exchange(true);
        }
        if (wake) {
            QMetaObject::invokeMethod(facade, [facade] { facade->drainNotices(); },
                                      Qt::QueuedConnection);
        }
    }

    // A pending UDP failure must not be hidden by a later success: the GUI still
    // has to react to the endpoint loss even if it recovered before the drain.
    // The slot therefore keeps two independent pieces of state inside the same
    // fixed entry: the failure that has not been consumed yet (`failurePending`
    // plus its message) and the current registration state (`flag`). A success
    // only updates the state; it never clears the failure.
    void notifyUdpReady(bool ok, const QString &message)
    {
        notifyGui(GuiNotice::UdpReady, [ok, message](GuiNoticeSlot &slot) {
            // notifyGui stamped this slot with the current room/media lifetime and
            // a lifetime change discards every queued lifecycle notice, so the
            // slot always describes the lifetime that is current here.
            slot.flag = ok;
            if (!ok) {
                slot.failurePending = true;
                slot.text = message;
            } else if (!slot.failurePending) {
                slot.text.clear();
            }
        });
    }

    // Starts a new room/media lifetime for queued notices. The generation is the
    // room lifetime (the token, session ID and room ID the registration proves);
    // a stream/profile switch inside the same room keeps proving the same
    // endpoint and therefore does not hide a pending endpoint failure. Queued
    // notices of the closed lifetime are discarded (they are never replayed as
    // state of the new session) and the generation advances so a notice a drain
    // already copied is recognized as stale before it is emitted.
    void resetUdpNoticeGeneration()
    {
        if (!m_plane) return;
        QMutexLocker lock(&m_plane->notices.mutex);
        advanceNoticeLifetimeLocked(m_plane->notices);
    }


    // Keeps the latest failure per operation so coalescing cannot hide a specific
    // failure (for example a failed create) behind a later ping timeout.
    static void mergeControlFailure(GuiNoticeSlot &slot, const QString &operation, int code,
                                    const QString &message)
    {
        for (auto &entry : slot.failures) {
            if (entry.operation != operation) continue;
            entry.code = code;
            entry.message = message;
            return;
        }
        if (slot.failures.size() >= MaxPendingControlFailures) slot.failures.removeFirst();
        slot.failures.append({operation, code, message});
    }

    void notifyUploadDropped(UploadDropCause cause, int packets)
    {
        if (packets <= 0) return;
        notifyGui(GuiNotice::UploadDropped, [this, cause, packets](GuiNoticeSlot &slot) {
            m_plane->notices.dropCauses[int(cause)] += quint64(packets);
            slot.value = packets;
        });
    }

    std::shared_ptr<DataPlane> m_plane;
    NetworkClient *m_facade = nullptr;
    QTcpSocket *m_tcp = nullptr;
    QUdpSocket *m_udp = nullptr;
    QTimer *m_tick = nullptr;
    QTimer *m_uploadTick = nullptr;
    QList<QByteArray> m_fragments;
    int m_fragmentNext = 0;
    QByteArray m_incoming;

    // Active publish packet. m_active covers the whole lifetime: waiting for the
    // whole-packet send budget, then fragment transmission, then release.
    // m_reservation is the only place that knows what the pacer granted and what
    // already reached the socket, so an abort refunds exactly the
    // reserved-but-unwritten remainder and never mints tokens for a packet that
    // was never admitted.
    bool m_active = false;
    bool m_sending = false;
    bool m_admissionWaitCounted = false;
    qint64 m_packetCost = 0;
    SendReservation m_reservation;
    qint64 m_activeEnqueuedUs = 0;
    qint64 m_admissionStartedUs = 0;
    qint64 m_admissionLimitUs = 0;
    qint64 m_sendStartedUs = 0;
    // Turn bookkeeping for the fragment loop: the observed interval between two
    // turns bounds how many fragments a turn has to write to stay inside the
    // active-send deadline.
    qint64 m_lastTurnUs = 0;
    qint64 m_turnIntervalUs = 0;
    int m_turnCount = 0;

    quint64 m_sessionId = 0;
    quint16 m_udpPort = 0;
    QHostAddress m_serverAddress;
    quint16 m_serverPort = 0;
    QString m_connectAddress;
    quint16 m_connectPort = 0;
    qint64 m_connectDeadlineUs = 0;
    qint64 m_nextPingUs = 0;
    // m_connecting covers the whole attempt, including the business hello that
    // follows TCP. m_connected turns true only after a valid hello response, so
    // an accepted socket with no plugin cannot look usable.
    bool m_connecting = false;
    bool m_connected = false;
    bool m_tcpConnected = false;
    bool m_stopped = false;

    quint32 m_nextRequestId = 1;
    PendingRequests m_pending;
    QMap<quint32, Operation> m_operations;

    bool m_inRoom = false;
    quint64 m_roomId = 0;
    QString m_roomCode;
    QString m_roomProfile;
    Role m_role = Role::Publisher;
    std::array<quint8, 16> m_token{};
    bool m_hasFormat = false;
    StreamFormat m_format;
    quint64 m_streamEpoch = 0;

    bool m_registered = false;
    bool m_registrationFailed = false;
    int m_registerAttempts = 0;
    qint64 m_nextRegisterUs = 0;
    // ACKs and live media update one activity timestamp on every observation.
    // Counter cursors must never consume a proof without advancing that time.
    MediaEndpointActivity m_endpointActivity;

    StreamReceiver m_receiver;
    quint64 m_lastExpired = 0;
    quint64 m_lastDependencyDropped = 0;
    quint64 m_lastDropped = 0;
    bool m_inboundRecovery = false;
    qint64 m_nextMailboxRecoveryUs = 0;
    ByteRateLimiter m_publishLimiter;
    double m_publishBytesPerSecond = 0;
    NetworkStats m_stats;
};

void NetworkEngine::begin()
{
    m_tcp = new QTcpSocket(this);
    m_udp = new QUdpSocket(this);
    // Never resolve an application proxy for the trust-network direct link.
    m_tcp->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    m_udp->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    m_tcp->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    if (!m_udp->bind(QHostAddress(QHostAddress::AnyIPv4), 0)) {
        // The control channel can still connect; registration will report the
        // missing media endpoint instead of silently pretending UDP is ready.
        m_registrationFailed = true;
    } else {
        m_udp->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 2 * 1024 * 1024);
    }
    connect(m_tcp, &QTcpSocket::connected, this, [this] { tcpConnected(); });
    connect(m_tcp, &QTcpSocket::readyRead, this, [this] { tcpReadyRead(); });
    connect(m_tcp, &QTcpSocket::disconnected, this, [this] {
        if (m_connected) {
            lost(QStringLiteral("控制连接已断开"));
        } else if (m_connecting) {
            failConnect(QStringLiteral("服务端在桌面共享握手完成前关闭了连接"));
        }
    });
    connect(m_tcp, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        if (!m_connected && m_connecting) {
            failConnect(m_tcp->errorString());
        } else if (m_connected && error != QAbstractSocket::RemoteHostClosedError) {
            lost(m_tcp->errorString());
        }
    });
    connect(m_udp, &QUdpSocket::readyRead, this, [this] { udpReadyRead(); });
    m_tick = new QTimer(this);
    m_tick->setInterval(20);
    connect(m_tick, &QTimer::timeout, this, [this] { tick(); });
    m_tick->start();
    m_uploadTick = new QTimer(this);
    m_uploadTick->setSingleShot(true);
    m_uploadTick->setTimerType(Qt::PreciseTimer);
    connect(m_uploadTick, &QTimer::timeout, this, [this] { onUploadTick(); });
    publishStats();
}

void NetworkEngine::stop()
{
    if (m_stopped) return;
    m_stopped = true;
    m_connecting = false;
    m_connected = false;
    m_tcpConnected = false;
    m_sessionId = 0;
    m_udpPort = 0;
    m_nextPingUs = 0;
    m_incoming.clear();
    if (m_tick) m_tick->stop();
    clearUpload();
    m_pending.clear();
    m_operations.clear();
    m_receiver.clear();
    clearRoom();
    if (m_tcp) {
        m_tcp->abort();
    }
    if (m_udp) {
        m_udp->close();
    }
    publishStats();
}

void NetworkEngine::publishStats()
{
    if (!m_plane) return;
    auto snapshot = m_stats;
    snapshot.localAddress = m_tcp ? m_tcp->localAddress().toString() : QString();
    snapshot.localUdpPort = m_udp ? m_udp->localPort() : 0;
    snapshot.serverUdpPort = m_udpPort;
    snapshot.mediaStreamEpoch = m_streamEpoch;
    snapshot.receiver = m_receiver.stats();
    {
        QMutexLocker lock(&m_plane->mutex);
        snapshot.outboundQueued = m_plane->outbound.count();
        snapshot.outboundBytes = m_plane->outbound.bytes();
        snapshot.inboundQueued = m_plane->inbound.count();
        snapshot.inboundBytes = m_plane->inbound.bytes();
        snapshot.outboundPeakBytes = m_plane->outboundPeakBytes;
        snapshot.outboundPeakPackets = m_plane->outboundPeakPackets;
        snapshot.inboundPeakBytes = m_plane->inboundPeakBytes;
        snapshot.inboundPeakPackets = m_plane->inboundPeakPackets;
        snapshot.uploadPacketsDroppedQueue = m_plane->uploadQueueRejected;
        snapshot.publishRateBytesPerSecond = quint64(m_publishBytesPerSecond);
        snapshot.publishBudgetBytes = m_publishLimiter.enabled() ? m_publishLimiter.available(nowUs()) : 0;
        m_plane->snapshot = snapshot;
    }
}

void NetworkEngine::connectTo(const QString &address, quint16 port)
{
    if (m_stopped) return;
    // Drop the previous attempt before aborting its socket: the synchronous
    // disconnected signal of that abort must not be reported as a failure of
    // the new attempt.
    m_connecting = false;
    m_connected = false;
    m_tcpConnected = false;
    if (m_tcp->state() != QAbstractSocket::UnconnectedState) {
        m_tcp->abort();
    }
    m_sessionId = 0;
    m_udpPort = 0;
    m_nextPingUs = 0;
    m_serverAddress = QHostAddress();
    m_serverPort = port;
    m_connectAddress = address;
    m_connectPort = port;
    m_incoming.clear();
    m_pending.clear();
    m_operations.clear();
    m_receiver.clear();
    clearRoom();
    m_connectDeadlineUs = nowUs() + ConnectTimeoutUs;
    m_connecting = true;
    if (diagnostics()) qInfo() << "zds network connect" << address << port;
    m_tcp->connectToHost(address, port);
    publishStats();
}

void NetworkEngine::disconnectFrom(const QString &reason)
{
    const bool wasAttempt = m_connecting || m_connected;
    // Reset the flags before aborting so the socket's synchronous
    // disconnected signal cannot be treated as an unexpected handshake loss.
    m_connected = false;
    m_connecting = false;
    m_tcpConnected = false;
    m_sessionId = 0;
    m_udpPort = 0;
    m_nextPingUs = 0;
    m_incoming.clear();
    if (m_tcp && m_tcp->state() != QAbstractSocket::UnconnectedState) m_tcp->abort();
    if (wasAttempt) {
        notifyGui(GuiNotice::ConnectionLost,
                  [reason](GuiNoticeSlot &slot) { slot.text = reason; });
    }
    m_pending.clear();
    m_operations.clear();
    m_receiver.clear();
    clearRoom();
    publishStats();
}

void NetworkEngine::failConnect(const QString &message)
{
    // Only a live attempt can fail; a second error after the unified cleanup
    // must not emit a duplicate connectFailed.
    if (!m_connecting) return;
    if (diagnostics()) qInfo() << "zds network connect failed" << message;
    m_connecting = false;
    m_connected = false;
    m_tcpConnected = false;
    m_sessionId = 0;
    m_udpPort = 0;
    m_nextPingUs = 0;
    m_incoming.clear();
    if (m_tcp) m_tcp->abort();
    m_pending.clear();
    m_operations.clear();
    m_receiver.clear();
    clearRoom();
    notifyGui(GuiNotice::ConnectFailed,
              [message](GuiNoticeSlot &slot) { slot.text = message; });
    publishStats();
}

void NetworkEngine::lost(const QString &reason)
{
    if (diagnostics()) qInfo() << "zds network lost" << reason;
    if (!m_connected) return;
    m_connected = false;
    m_connecting = false;
    m_tcpConnected = false;
    m_sessionId = 0;
    m_udpPort = 0;
    m_nextPingUs = 0;
    m_incoming.clear();
    if (m_tcp) m_tcp->abort();
    m_pending.clear();
    m_operations.clear();
    m_receiver.clear();
    clearRoom();
    notifyGui(GuiNotice::ConnectionLost,
              [reason](GuiNoticeSlot &slot) { slot.text = reason; });
    publishStats();
}

void NetworkEngine::tcpConnected()
{
    if (m_stopped) return;
    // TCP is up, but this is not yet a usable business connection: m_connecting
    // stays true and the deadline keeps running until a valid hello arrives.
    m_tcpConnected = true;
    m_serverAddress = m_tcp->peerAddress();
    m_serverPort = m_tcp->peerPort();
    if (diagnostics())
        qInfo() << "zds network tcp connected" << m_serverAddress << m_serverPort
                << "udpBound" << (m_udp && m_udp->state() == QAbstractSocket::BoundState);
    if (m_serverAddress.protocol() != QAbstractSocket::IPv4Protocol) {
        failConnect(QStringLiteral("需要 IPv4 服务端地址"));
        return;
    }
    if (sendRequest(Operation::Hello, [](Request &) {}) == 0) {
        failConnect(QStringLiteral("无法发送桌面共享握手请求"));
        return;
    }
    publishStats();
}

void NetworkEngine::tcpReadyRead()
{
    if (!m_tcp) return;
    m_incoming.append(m_tcp->readAll());
    for (;;) {
        protocol::Packet packet;
        const auto result = protocol::takeFrame(m_incoming, packet);
        if (result == protocol::ReadResult::Incomplete) return;
        if (result == protocol::ReadResult::Invalid) {
            if (!m_connected && m_connecting) {
                failConnect(QStringLiteral("服务端在桌面共享握手阶段返回无效控制帧"));
            } else {
                lost(QStringLiteral("控制帧无效"));
            }
            return;
        }
        if (packet.plugin == QByteArray(BuiltinRoute)) {
            handleBuiltin(packet.payload);
            continue;
        }
        // Other plugin routes are not part of this connection attempt; only a
        // missing/unready answer for our own route may affect the attempt.
        if (packet.plugin != QByteArray(Route)) continue;
        if (!handleControl(packet.payload)) {
            if (!m_connected && m_connecting) {
                failConnect(QStringLiteral("服务端在桌面共享握手阶段返回无效控制消息"));
            } else {
                lost(QStringLiteral("控制消息无效"));
            }
            return;
        }
    }
}

void NetworkEngine::handleBuiltin(const QByteArray &payload)
{
    // A host without a ready desktop-share plugin answers the route-specific
    // hello with its own `builtin` route instead of the plugin. This is the
    // only builtin message that describes this attempt.
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (diagnostics()) qInfo() << "zds network invalid builtin message" << payload.size();
        return;
    }
    const auto object = document.object();
    const auto okValue = object.value(QStringLiteral("ok"));
    const auto errorValue = object.value(QStringLiteral("error"));
    const auto pluginValue = object.value(QStringLiteral("plugin"));
    if (okValue.isBool() && okValue.toBool()) return;
    if (!errorValue.isString() || !pluginValue.isString()) return;
    if (pluginValue.toString() != QLatin1String(Route)) return;
    if (errorValue.toString() != QLatin1String("unknown_or_unready_plugin")) return;
    if (!m_connecting && !m_connected) return;
    const auto message =
        QStringLiteral("服务端未安装或未就绪桌面共享插件（desktop-share）：宿主返回 %1。")
            .arg(errorValue.toString());
    if (diagnostics()) qInfo() << "zds network builtin" << message;
    if (m_connecting) {
        failConnect(message);
    } else {
        lost(message);
    }
}

bool NetworkEngine::handleControl(const QByteArray &payload)
{
    Event event;
    const auto eventResult = decodeEvent(payload, event);
    if (eventResult.ok) {
        handleEvent(event);
        return true;
    }
    // Correlate by the request ID first: a response can only be decoded with
    // the operation that produced it, because the shared decoder validates the
    // operation-specific data shape.
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (diagnostics()) qInfo() << "zds network undecodable response" << payload.size();
        return false;
    }
    const auto identifier = document.object().value(QStringLiteral("id"));
    if (!identifier.isDouble()) return false;
    const auto id = quint32(identifier.toDouble());
    const auto it = m_operations.find(id);
    if (it == m_operations.end()) {
        ++m_stats.lateResponses;
        return true;
    }
    const auto operation = it.value();
    Response response;
    if (!decodeResponse(payload, operation, response).ok) {
        if (diagnostics())
            qInfo() << "zds network invalid response" << operationName(operation) << payload.size();
        return false;
    }
    m_operations.erase(it);
    if (!m_pending.complete(response.id, nowMs())) {
        // A response that arrives at or after the five-second deadline is an
        // old response and must not change the new state.
        ++m_stats.lateResponses;
        return true;
    }
    ++m_stats.tcpResponsesReceived;
    if (diagnostics()) qInfo() << "zds network response" << operationName(operation) << response.ok;
    handleResponse(response, operation);
    return true;
}

void NetworkEngine::handleResponse(const Response &response, Operation operation)
{
    if (!response.ok) {
        const auto operationName_ = operationName(operation);
        if (operation == Operation::Hello) {
            failConnect(QStringLiteral("服务端拒绝握手：%1").arg(response.message));
            return;
        }
        notifyGui(GuiNotice::ControlFailed,
                  [operationName_, code = int(response.error), message = response.message](
                      GuiNoticeSlot &slot) {
                      mergeControlFailure(slot, operationName_, code, message);
                  });
        if (operation == Operation::StreamFeedback) {
            // The acknowledgement (or the rejection) completes this feedback
            // round. The controller paces the next report from this point, so
            // the server never sees two reports inside its one-second window
            // because of a shorter network trip.
            notifyGui(GuiNotice::FeedbackCompleted,
                      [code = int(response.error)](GuiNoticeSlot &slot) {
                          slot.flag = false;
                          slot.code = code;
                      });
        }
        if (operation == Operation::Create || operation == Operation::Join) {
            clearRoom();
        }
        return;
    }
    switch (operation) {
    case Operation::Hello:
        // The business handshake is complete only now; the attempt's deadline
        // no longer applies and keepalives start from this point.
        m_connecting = false;
        m_connected = true;
        m_tcpConnected = true;
        m_connectDeadlineUs = 0;
        m_nextPingUs = nowUs() + KeepaliveUs;
        m_sessionId = response.hello.sessionId;
        m_udpPort = response.hello.udpPort;
        notifyGui(GuiNotice::Connected,
                  [session = m_sessionId, port = m_udpPort](GuiNoticeSlot &slot) {
                      slot.first = session;
                      slot.second = port;
                  });
        break;
    case Operation::Create:
        adoptRoom(response.room);
        notifyGui(GuiNotice::RoomCreated,
                  [room = response.room](GuiNoticeSlot &slot) { slot.room = room; });
        startRegistration();
        break;
    case Operation::Join:
        adoptRoom(response.room);
        notifyGui(GuiNotice::RoomJoined,
                  [room = response.room](GuiNoticeSlot &slot) { slot.room = room; });
        startRegistration();
        break;
    case Operation::Leave:
    case Operation::Stop:
        clearRoom();
        break;
    case Operation::StreamConfigure:
        m_streamEpoch = response.streamEpoch;
        m_format.streamEpoch = m_streamEpoch;
        m_hasFormat = true;
        notifyGui(GuiNotice::StreamConfigured,
                  [epoch = m_streamEpoch](GuiNoticeSlot &slot) { slot.first = epoch; });
        break;
    case Operation::StreamReady:
        notifyGui(GuiNotice::StreamReadyConfirmed,
                  [epoch = response.streamEpoch](GuiNoticeSlot &slot) { slot.first = epoch; });
        break;
    case Operation::StreamFeedback:
        notifyGui(GuiNotice::FeedbackCompleted,
                  [](GuiNoticeSlot &slot) { slot.flag = true; });
        break;
    case Operation::StreamKeyframe:
    case Operation::Ping:
        break;
    }
    publishStats();
}

void NetworkEngine::handleEvent(const Event &event)
{
    switch (event.kind) {
    case EventKind::RoomState:
        notifyGui(GuiNotice::RoomStateChanged,
                  [room = event.roomId, state = int(event.roomState),
                   viewers = event.viewerCount](GuiNoticeSlot &slot) {
                      slot.first = room;
                      slot.code = state;
                      slot.value = viewers;
                  });
        break;
    case EventKind::StreamConfig:
        if (m_roomId == event.roomId) {
            m_format = event.format;
            m_streamEpoch = event.format.streamEpoch;
            m_hasFormat = true;
        }
        notifyGui(GuiNotice::StreamConfig,
                  [room = event.roomId, format = event.format](GuiNoticeSlot &slot) {
                      slot.first = room;
                      slot.format = format;
                  });
        break;
    case EventKind::StreamKeyframe:
        notifyGui(GuiNotice::StreamKeyframe,
                  [room = event.roomId, epoch = event.streamEpoch](GuiNoticeSlot &slot) {
                      slot.first = room;
                      slot.second = epoch;
                  });
        break;
    case EventKind::RoomClosed:
        if (m_roomId == event.roomId) clearRoom();
        notifyGui(GuiNotice::RoomClosed,
                  [room = event.roomId, reason = int(event.reason)](GuiNoticeSlot &slot) {
                      slot.first = room;
                      slot.code = reason;
                  });
        break;
    }
}

void NetworkEngine::adoptRoom(const RoomInfo &room)
{
    m_inRoom = true;
    m_roomId = room.roomId;
    m_roomCode = room.roomCode;
    m_roomProfile = room.profile;
    m_role = room.role;
    m_token.fill(0);
    if (room.token.size() == 16) {
        std::memcpy(m_token.data(), room.token.constData(), 16);
    }
    m_hasFormat = room.hasFormat;
    m_format = room.format;
    m_streamEpoch = room.hasFormat ? room.format.streamEpoch : 0;
    m_receiver.clear();
    m_registered = false;
    m_registrationFailed = false;
    m_registerAttempts = 0;
    m_nextRegisterUs = 0;
    m_endpointActivity.reset(m_stats.datagramsSent, m_stats.inboundDatagrams);
    m_stats.udpRegistersSent = 0;
    m_stats.udpAckReceived = 0;
    // A new room is a new media lifetime: readiness notices of the previous room
    // must not be delivered as the state of this one.
    resetUdpNoticeGeneration();
}

void NetworkEngine::clearRoom()
{
    clearUpload();
    clearReceive();
    m_inRoom = false;
    m_roomId = 0;
    m_roomCode.clear();
    m_roomProfile.clear();
    m_token.fill(0);
    m_hasFormat = false;
    m_format = {};
    m_streamEpoch = 0;
    m_registered = false;
    m_registrationFailed = false;
    m_registerAttempts = 0;
    m_nextRegisterUs = 0;
    m_endpointActivity.reset(m_stats.datagramsSent, m_stats.inboundDatagrams);
    m_receiver.clear();
    // The room is gone, so a queued failure/recovery of its media endpoint is
    // stale as well. It is discarded here instead of surfacing in the next room.
    resetUdpNoticeGeneration();
}

void NetworkEngine::create(const QString &profile)
{
    if (!m_connected) {
        reportControlFailure(QStringLiteral("create"), ErrorCode::NotReady,
                             QStringLiteral("尚未连接服务端"));
        return;
    }
    sendRequest(Operation::Create, [&profile](Request &request) { request.profile = profile; });
}

void NetworkEngine::join(const QString &code)
{
    if (!m_connected) {
        reportControlFailure(QStringLiteral("join"), ErrorCode::NotReady,
                             QStringLiteral("尚未连接服务端"));
        return;
    }
    sendRequest(Operation::Join, [&code](Request &request) { request.roomCode = code; });
}

void NetworkEngine::leave(const QString &reason)
{
    if (!m_inRoom) {
        clearRoom();
        return;
    }
    sendRequest(Operation::Leave, [this](Request &request) { request.roomId = m_roomId; });
    Q_UNUSED(reason);
    clearRoom();
}

void NetworkEngine::stopRoom(const QString &reason)
{
    if (!m_inRoom) {
        clearRoom();
        return;
    }
    sendRequest(Operation::Stop, [this](Request &request) { request.roomId = m_roomId; });
    Q_UNUSED(reason);
    clearRoom();
}

void NetworkEngine::configure(const StreamFormat &format)
{
    if (!m_connected || !m_inRoom) {
        reportControlFailure(QStringLiteral("stream.configure"), ErrorCode::NotReady,
                             QStringLiteral("当前不在房间中"));
        return;
    }
    // The server sends stream.config to viewers only. Retain the publisher's
    // negotiated codec locally as well, so admission uses the codec's actual
    // anchor semantics instead of the empty format from room creation.
    m_format = format;
    sendRequest(Operation::StreamConfigure, [this, &format](Request &request) {
        request.roomId = m_roomId;
        request.format = format;
    });
    // Publish byte pacing: profile bitrate x 1.25 with a 1 MiB burst. The
    // limiter is reset for each new stream configuration and a whole packet is
    // reserved before it starts, so a large packet waits for budget instead of
    // dripping past the receiver's reassembly deadline. The queue never grows to
    // absorb the waiting; it drops packets that exceed the bounded budget.
    if (format.bitrate > 0) {
        m_publishBytesPerSecond = double(format.bitrate) / 8.0 * 1.25;
        m_publishLimiter.reset(m_publishBytesPerSecond, 1024 * 1024, nowUs());
        m_admissionLimitUs =
            qint64(std::ceil(double(m_publishLimiter.capacity()) / m_publishBytesPerSecond
                             * 1'000'000.0))
            + AdmissionSlackUs;
    } else {
        m_admissionLimitUs = 0;
    }
}

void NetworkEngine::ready(quint64 epoch)
{
    if (!m_connected || !m_inRoom || epoch == 0) return;
    sendRequest(Operation::StreamReady, [this, epoch](Request &request) {
        request.roomId = m_roomId;
        request.streamEpoch = epoch;
    });
}

void NetworkEngine::keyframe(quint64 epoch)
{
    if (!m_connected || !m_inRoom || epoch == 0) return;
    sendRequest(Operation::StreamKeyframe, [this, epoch](Request &request) {
        request.roomId = m_roomId;
        request.streamEpoch = epoch;
    });
}

void NetworkEngine::feedback(const Feedback &values, quint64 epoch)
{
    if (!m_connected || !m_inRoom || epoch == 0) return;
    sendRequest(Operation::StreamFeedback, [this, values, epoch](Request &request) {
        request.roomId = m_roomId;
        request.streamEpoch = epoch;
        request.feedback = values;
    });
}

void NetworkEngine::beginReceive(quint64 epoch)
{
    if (!m_inRoom || epoch == 0) return;
    clearReceive();
    MediaHeader identity;
    identity.type = MediaType::Video;
    identity.token = m_token;
    identity.sessionId = m_sessionId;
    identity.roomId = m_roomId;
    identity.streamEpoch = epoch;
    // Both codecs wait for an initial anchor. After that, absolute tile records
    // can survive gaps and reordering; treating every baseline chunk like a
    // complete H.264 key frame abandons earlier chunks of the same baseline.
    m_receiver.configure(identity, m_format.codec != QStringLiteral("bgra-tiles"), nowUs());
    m_inboundRecovery = false;
    m_nextMailboxRecoveryUs = 0;
    publishStats();
}

void NetworkEngine::resyncReceive()
{
    m_receiver.resync();
    m_inboundRecovery = true;
    m_nextMailboxRecoveryUs = nowUs() + 1000000;
    if (m_plane) {
        QMutexLocker lock(&m_plane->mutex);
        m_plane->inbound.clear();
    }
    publishStats();
}

void NetworkEngine::clearReceive()
{
    m_receiver.clear();
    m_inboundRecovery = false;
    if (m_plane) {
        QMutexLocker lock(&m_plane->mutex);
        m_plane->inbound.clear();
    }
    publishStats();
}

void NetworkEngine::clearUpload()
{
    if (!m_plane) return;
    if (m_uploadTick) m_uploadTick->stop();
    // A clear (source/profile switch) or stop also ends the active packet: its
    // reserved-but-unwritten bytes go back exactly once, so clearing the queue
    // cannot leave the pacer charged for a packet that will never be written.
    releaseReservation();
    m_fragments.clear();
    m_fragmentNext = 0;
    m_active = false;
    m_sending = false;
    m_packetCost = 0;
    m_sendStartedUs = 0;
    m_admissionStartedUs = 0;
    m_admissionWaitCounted = false;
    QMutexLocker lock(&m_plane->mutex);
    m_plane->outbound.clear();
    m_plane->wakeOutbound.store(false);
    // Report what the queue abandoned before the clear; the counters are
    // cumulative, so the deltas are what this transition discarded.
    reportQueueDropsLocked();
}

quint32 NetworkEngine::sendRequest(Operation operation, const std::function<void(Request &)> &fill)
{
    Request request;
    request.id = m_nextRequestId++;
    if (m_nextRequestId == 0) m_nextRequestId = 1;
    request.op = operation;
    fill(request);
    const auto payload = encodeRequest(request);
    if (payload.isEmpty()) {
        reportControlFailure(operationName(operation), ErrorCode::InvalidRequest,
                             QStringLiteral("控制请求无法编码"));
        return 0;
    }
    if (!m_pending.add(request.id, nowMs())) {
        reportControlFailure(operationName(operation), ErrorCode::RateLimited,
                             QStringLiteral("未完成请求过多"));
        return 0;
    }
    m_operations.insert(request.id, operation);
    const auto framed = protocol::frame(QByteArray(Route), payload);
    if (framed.isEmpty() || m_tcp->write(framed) != framed.size()) {
        m_pending.complete(request.id, nowMs());
        m_operations.remove(request.id);
        reportControlFailure(operationName(operation), ErrorCode::Internal,
                             QStringLiteral("控制帧写入失败"));
        return 0;
    }
    ++m_stats.tcpRequestsSent;
    return request.id;
}

void NetworkEngine::reportControlFailure(const QString &operation, ErrorCode code,
                                         const QString &message)
{
    ++m_stats.controlErrors;
    notifyGui(GuiNotice::ControlFailed,
              [operation, code = int(code), message](GuiNoticeSlot &slot) {
                  mergeControlFailure(slot, operation, code, message);
              });
}

void NetworkEngine::startRegistration()
{
    if (!m_inRoom) return;
    if (m_udp->state() != QAbstractSocket::BoundState) m_registrationFailed = true;
    m_registered = false;
    m_registerAttempts = 0;
    m_nextRegisterUs = nowUs();
    m_endpointActivity.reset(m_stats.datagramsSent, m_stats.inboundDatagrams);
}

void NetworkEngine::sendRegister(bool keepalive)
{
    if (!m_inRoom || m_sessionId == 0 || m_roomId == 0) return;
    const auto payload = encodeRegister(m_token, m_sessionId, m_roomId);
    if (payload.isEmpty()) return;
    const auto datagram = wrapZts1(payload);
    if (datagram.isEmpty()) return;
    if (m_udp->writeDatagram(datagram, m_serverAddress, m_udpPort) != datagram.size()) {
        return;
    }
    if (keepalive) {
        ++m_stats.keepalivesSent;
        m_nextRegisterUs = nowUs() + KeepaliveUs;
    } else {
        ++m_stats.udpRegistersSent;
        ++m_registerAttempts;
        m_nextRegisterUs = nowUs() + RegisterRetryUs;
    }
}

void NetworkEngine::udpReadyRead()
{
    if (!m_udp) return;
    while (m_udp->hasPendingDatagrams()) {
        const auto datagram = m_udp->receiveDatagram();
        if (datagram.senderAddress() != m_serverAddress || datagram.senderPort() != m_udpPort) {
            ++m_stats.inboundRejected;
            continue;
        }
        QByteArray payload;
        if (!unwrapZts1(datagram.data(), payload)) {
            ++m_stats.inboundRejected;
            continue;
        }
        DecodedDatagram decoded;
        if (!decodeDatagram(payload, decoded)) {
            ++m_stats.inboundRejected;
            continue;
        }
        if (decoded.header.type == MediaType::RegisterAck) {
            if (decoded.header.token == m_token && decoded.header.sessionId == m_sessionId
                && decoded.header.roomId == m_roomId) {
                handleAck();
            } else {
                ++m_stats.inboundRejected;
            }
            continue;
        }
        if (decoded.header.type != MediaType::Video || !m_registered) {
            ++m_stats.inboundRejected;
            continue;
        }
        ++m_stats.inboundDatagrams;
        if (diagnostics() && decoded.header.fragmentIndex == 0
            && (decoded.header.flags & quint16(MediaFlag::KeyFrame)))
            qInfo() << "zds key fragment" << decoded.header.sequence << "bytes"
                    << decoded.header.packetBytes << "fragments" << decoded.header.fragmentCount;
        QList<VideoPacket> completed;
        const auto result = m_receiver.push(payload, nowUs(), completed);
        if (diagnostics() && result == AssemblyResult::Complete)
            qInfo() << "zds complete" << decoded.header.sequence << "bytes"
                    << decoded.header.packetBytes << "key" << decoded.header.flags
                    << "outputs" << completed.size() << "late" << m_receiver.stats().latePackets;
        if (result == AssemblyResult::Rejected) ++m_stats.inboundRejected;
        for (auto &packet : completed) deliverInbound(std::move(packet));
    }
    publishStats();
}

void NetworkEngine::handleAck()
{
    if (!m_inRoom) return;
    m_registered = true;
    m_registrationFailed = false;
    const auto acknowledgedAt = nowUs();
    m_endpointActivity.acknowledge(acknowledgedAt);
    m_nextRegisterUs = acknowledgedAt + KeepaliveUs;
    ++m_stats.udpAckReceived;
    notifyUdpReady(true, QString());
    publishStats();
}

void NetworkEngine::deliverInbound(VideoPacket packet)
{
    if (!m_plane) return;
    const qint64 now = nowUs();
    if (m_inboundRecovery) {
        if (!packet.keyFrame()) return;
        packet.flags |= quint16(MediaFlag::Discontinuity);
    }
    bool delivered = false;
    {
        QMutexLocker lock(&m_plane->mutex);
        if (!m_plane->closed) delivered = m_plane->inbound.push(std::move(packet), now);
        if (delivered) {
            m_plane->inboundPeakPackets = std::max(m_plane->inboundPeakPackets,
                                                   m_plane->inbound.count());
            m_plane->inboundPeakBytes = std::max(m_plane->inboundPeakBytes,
                                                 m_plane->inbound.bytes());
        }
    }
    if (!delivered) {
        ++m_stats.inboundDroppedBudget;
        m_inboundRecovery = true;
        m_receiver.resync();
        if (now >= m_nextMailboxRecoveryUs) {
            m_nextMailboxRecoveryUs = now + 1000000;
            notifyGui(GuiNotice::StreamGap, [](GuiNoticeSlot &) {});
        }
        return;
    }
    m_inboundRecovery = false;
    ++m_stats.inboundDelivered;
    if (!m_plane->wakeInbound.exchange(true)) {
        NetworkClient *facade = m_facade;
        if (facade) {
            QMetaObject::invokeMethod(facade, [facade] { facade->drainInbound(); },
                                      Qt::QueuedConnection);
        }
    }
}

void NetworkEngine::drainOutbound()
{
    if (!m_plane || m_stopped) return;
    if (m_active) return;
    VideoPacket packet;
    qint64 enqueuedUs = 0;
    bool available = false;
    {
        QMutexLocker lock(&m_plane->mutex);
        available = m_plane->outbound.acquire(nowUs(), packet, &enqueuedUs);
        // The queue was drained: allow the next push to post another wakeup. A
        // packet that completes re-arms the drain, so no wakeup is lost.
        m_plane->wakeOutbound.store(false);
        reportQueueDropsLocked();
    }
    if (!available) {
        publishStats();
        return;
    }
    m_active = true;
    m_sending = false;
    m_admissionWaitCounted = false;
    m_packetCost = 0;
    m_sendStartedUs = 0;
    m_admissionStartedUs = nowUs();
    beginActive(std::move(packet), enqueuedUs);
    publishStats();
}

// Turns one acquired queue entry into the fragment list of the active packet and
// decides whether it may start now. A packet is admitted as a whole: the complete
// wire cost is reserved from the pacer before the first fragment is written, so a
// packet either finishes inside the receiver's reassembly window or is not sent at
// all. The old per-fragment pacing could drip a large key frame over seconds while
// the receiver had already expired it.
void NetworkEngine::beginActive(VideoPacket packet, qint64 enqueuedUs)
{
    m_activeEnqueuedUs = enqueuedUs;
    if (!m_registered || !m_inRoom || m_role != Role::Publisher || m_streamEpoch == 0
        || (packet.streamEpoch != 0 && packet.streamEpoch != m_streamEpoch)) {
        // The stream identity no longer allows publishing this packet. Stop
        // before any fragment is written; the stream switch itself clears the
        // queue, so this is not a dependency break of the current stream.
        ++m_stats.uploadPacketsDroppedBudget;
        finishActiveAborted(QStringLiteral("当前流不可发布"), UploadDropCause::NotPublishable);
        return;
    }
    const bool keyFrame = packet.keyFrame();
    const int payloadBytes = packet.data.size();
    VideoPacket outgoing = packet;
    outgoing.token = m_token;
    outgoing.sessionId = m_sessionId;
    outgoing.roomId = m_roomId;
    outgoing.streamEpoch = m_streamEpoch;
    m_fragments = fragmentVideo(outgoing);
    m_fragmentNext = 0;
    if (m_fragments.isEmpty()) {
        ++m_stats.uploadPacketsDroppedBudget;
        finishActiveAborted(QStringLiteral("编码包无法分片"), UploadDropCause::Fragment);
        return;
    }
    m_packetCost = 0;
    for (const auto &fragment : m_fragments) m_packetCost += datagramBytes(fragment.size());
    if (diagnostics())
        qInfo() << "zds upload admit" << packet.sequence << "bytes" << payloadBytes
                << "fragments" << m_fragments.size() << "cost" << m_packetCost
                << "key" << keyFrame << "burst" << m_publishLimiter.capacity();
    retryAdmission();
}

// True while a queued packet is an independent key frame, i.e. a fresher anchor
// that fully supersedes a packet still waiting for its send budget.
bool NetworkEngine::newerKeyFrameWaiting()
{
    if (!m_plane) return false;
    if (m_format.codec == QStringLiteral("bgra-tiles")) return false;
    QMutexLocker lock(&m_plane->mutex);
    return m_plane->outbound.queuedKeyFrame();
}

// Waits for the whole-packet budget. The wait is bounded by the time the bucket
// needs to refill from empty to the burst, which is also the longest a legal
// packet can need; a packet larger than the burst can never complete inside the
// reassembly window and is dropped instead of dripping fragment by fragment.
void NetworkEngine::retryAdmission()
{
    if (m_stopped || !m_active || m_sending) return;
    if (!m_publishLimiter.enabled()) {
        startSending();
        return;
    }
    const qint64 now = nowUs();
    if (m_packetCost > m_publishLimiter.capacity()) {
        ++m_stats.uploadPacketsDroppedPacing;
        finishActiveAborted(QStringLiteral("编码包超出发送突发上限"), UploadDropCause::Pacing);
        return;
    }
    // Hard bound for a deferred admission: waiting can never help once the packet
    // has waited longer than the bucket needs to refill from empty to full. This
    // runs before the take so a packet that sat through a stalled event loop is
    // dropped instead of silently started late.
    if (m_admissionLimitUs > 0 && now - m_admissionStartedUs > m_admissionLimitUs) {
        ++m_stats.uploadPacketsDroppedPacing;
        finishActiveAborted(QStringLiteral("等待发送预算超时"),
                            UploadDropCause::AdmissionTimeout);
        return;
    }
    // Freshness and supersession are decided before a single token is taken, so
    // they cover both admission paths. A packet that has not written a fragment
    // must not hold the single send slot in front of fresher content: a queued
    // key frame is an independent anchor that supersedes everything which has
    // not started, and any other newer packet makes an over-age admission stale
    // (the 200 ms publication age applies to the wait itself and is not bypassed
    // by calling the packet active). Doing this after take() would consume the
    // refilled budget first and transmit content that a newer key frame has
    // already superseded.
    {
        bool superseded = false;
        bool newerQueued = false;
        if (m_plane) {
            QMutexLocker lock(&m_plane->mutex);
            // Each tile baseline packet carries different absolute regions;
            // a later chunk cannot supersede the chunk still being admitted.
            superseded = m_format.codec != QStringLiteral("bgra-tiles")
                         && m_plane->outbound.queuedKeyFrame();
            newerQueued = m_plane->outbound.queuedPackets() > 0;
        }
        if (superseded || (newerQueued && now - m_activeEnqueuedUs > PublishQueue::MaxAgeUs)) {
            ++m_stats.uploadPacketsDroppedAge;
            finishActiveAborted(
                superseded ? QStringLiteral("新关键帧取代尚未开始的编码包")
                           : QStringLiteral("等待发送预算期间超出发送年龄"),
                UploadDropCause::AdmissionAge);
            return;
        }
    }
    // A packet that can start right now is admitted immediately; the pacer was
    // never the problem for it.
    const qint64 waitUs = m_publishLimiter.take(m_packetCost, now);
    if (waitUs == 0) {
        // The whole packet cost is now reserved exactly once; the abort/complete
        // paths refund only what was reserved and not yet written.
        if (!m_reservation.reserve(m_packetCost)) {
            ++m_stats.uploadPacketsDroppedBudget;
            finishActiveAborted(QStringLiteral("发送预留状态异常"), UploadDropCause::Fragment);
            return;
        }
        startSending();
        return;
    }
    if (!m_admissionWaitCounted) {
        m_admissionWaitCounted = true;
        ++m_stats.uploadPacketsDeferred;
        ++m_stats.pacingWaits;
    }
    const auto waitMs =
        qBound<qint64>(1, (qMin(waitUs, qint64(UploadRetryMaxMs) * 1000) + 999) / 1000,
                       qint64(UploadRetryMaxMs));
    m_uploadTick->start(int(waitMs));
}

void NetworkEngine::startSending()
{
    if (m_stopped || !m_active || m_sending || m_fragments.isEmpty()) return;
    m_sending = true;
    m_sendStartedUs = nowUs();
    m_lastTurnUs = m_sendStartedUs;
    m_turnCount = 1;
    ++m_stats.uploadPackets;
    if (diagnostics())
        qInfo() << "zds upload start" << "fragments" << m_fragments.size() << "cost"
                << m_packetCost << "waitedUs" << (m_sendStartedUs - m_admissionStartedUs);
    m_uploadTick->start(UploadTurnMs);
}

// One single-shot timer drives both the budget retry of a deferred admission and
// the fragment turns of a started packet; the two states never overlap.
void NetworkEngine::onUploadTick()
{
    if (m_stopped) return;
    if (m_sending) {
        sendBurst();
    } else {
        retryAdmission();
    }
}

// Fragments to write in this turn. The baseline batch spreads a baseline over
// event-loop turns (sending 100+ fragments in one turn reproducibly loses part
// of the first 1080p baseline on the real host). The batch adapts upwards only
// when the observed turn interval says the remaining fragments would no longer
// fit inside the active-send deadline: a single-shot 1 ms timer is delivered on
// the system clock tick, so on a host whose effective tick is the coarse
// 15.625 ms default the 50 turns such a packet needs cannot fit into 150 ms and
// the packet used to be aborted after 9 turns (144 fragments). Scaling the batch
// by the measured turn interval keeps the whole packet inside the same deadline
// without changing the deadline itself, the pace or the burst.
int NetworkEngine::fragmentBatch(qint64 now) const
{
    const int remaining = int(m_fragments.size()) - m_fragmentNext;
    if (remaining <= UploadFragmentsPerTurn) return remaining;
    const qint64 turnUs = m_turnIntervalUs;
    if (m_turnCount < 2 || turnUs <= 0) return UploadFragmentsPerTurn;
    const qint64 left = ActiveSendDeadlineUs - (now - m_sendStartedUs);
    if (left <= 0) return UploadFragmentsPerTurn;
    const qint64 turnsLeft = std::max<qint64>(1, left / turnUs);
    const qint64 needed = (qint64(remaining) + turnsLeft - 1) / turnsLeft;
    const qint64 lowest = UploadFragmentsPerTurn;
    const qint64 highest = UploadFragmentsPerTurnMax;
    return int(qBound(lowest, needed, highest));
}

void NetworkEngine::sendBurst()
{
    if (m_stopped || !m_active) return;
    if (!m_sending) {
        retryAdmission();
        return;
    }
    if (m_fragments.isEmpty()) return;
    const qint64 turnUs = nowUs();
    if (m_lastTurnUs > 0) m_turnIntervalUs = turnUs - m_lastTurnUs;
    if (m_turnIntervalUs > UploadSlowTurnUs && diagnostics())
        qInfo() << "zds upload slow turn" << "turnUs" << m_turnIntervalUs
                << "atFragment" << m_fragmentNext << "of" << m_fragments.size()
                << "deadlineLeftUs" << (ActiveSendDeadlineUs - (turnUs - m_sendStartedUs));
    m_lastTurnUs = turnUs;
    ++m_turnCount;
    const int end = std::min(m_fragmentNext + fragmentBatch(turnUs), int(m_fragments.size()));
    while (m_fragmentNext < end) {
        // The whole packet was admitted together, so a delay here means the
        // event loop or the socket stalled. Continuing would only produce
        // fragments the receiver has already expired.
        if (m_sendStartedUs > 0 && nowUs() - m_sendStartedUs > ActiveSendDeadlineUs) {
            ++m_stats.uploadPacketsDroppedDeadline;
            finishActiveAborted(QStringLiteral("超出重组发送期限"), UploadDropCause::SendDeadline);
            return;
        }
        const auto datagram = wrapZts1(m_fragments.at(m_fragmentNext));
        if (datagram.isEmpty()) {
            // The fragment never reached the socket; its reserved cost is part of
            // the refund below, so it must not be counted as written.
            ++m_stats.datagramsDropped;
            ++m_stats.uploadPacketsDroppedBudget;
            finishActiveAborted(QStringLiteral("UDP 编码失败"), UploadDropCause::Encode);
            return;
        }
        if (m_udp->writeDatagram(datagram, m_serverAddress, m_udpPort) != datagram.size()) {
            // One missing fragment already makes the whole packet unusable for
            // the receiver; drop the rest instead of spending the budget on it.
            ++m_stats.datagramsDropped;
            finishActiveAborted(QStringLiteral("UDP 发送失败"), UploadDropCause::Socket);
            return;
        }
        ++m_fragmentNext;
        m_reservation.written(datagram.size());
        ++m_stats.datagramsSent;
    }
    if (m_fragmentNext < m_fragments.size()) {
        m_uploadTick->start(UploadTurnMs);
    } else {
        finishActivePacket();
    }
}

void NetworkEngine::finishActivePacket()
{
    if (diagnostics())
        qInfo() << "zds upload done" << "fragments" << m_fragments.size() << "turns"
                << m_turnCount << "spanUs" << (m_sendStartedUs > 0 ? nowUs() - m_sendStartedUs
                                                                  : qint64(0));
    m_fragments.clear();
    m_fragmentNext = 0;
    m_sending = false;
    m_active = false;
    m_packetCost = 0;
    m_reservation.complete();
    m_sendStartedUs = 0;
    {
        QMutexLocker lock(&m_plane->mutex);
        m_plane->outbound.releaseActive();
        reportQueueDropsLocked();
    }
    QMetaObject::invokeMethod(this, [this] { drainOutbound(); }, Qt::QueuedConnection);
}

// Returns the reserved-but-unwritten part of the active reservation. Only the
// bytes that were actually granted by the pacer and never accepted by the socket
// are refunded, so a rejected admission cannot mint rate credit.
void NetworkEngine::releaseReservation()
{
    // Only what the pacer granted and the socket never accepted goes back. A
    // packet rejected before admission has no reservation, so this returns 0 and
    // cannot create rate credit.
    const qint64 unsent = m_reservation.release();
    if (unsent > 0) m_publishLimiter.refund(unsent, nowUs());
}

// Drops the active packet and retracts every queued dependent: the wire must not
// receive the delta packets that reference a packet the viewer never completes.
void NetworkEngine::finishActiveAborted(const QString &reason, UploadDropCause cause)
{
    if (!m_active) return;
    if (m_uploadTick) m_uploadTick->stop();
    releaseReservation();
    m_fragments.clear();
    m_fragmentNext = 0;
    m_sending = false;
    m_active = false;
    m_packetCost = 0;
    m_sendStartedUs = 0;
    {
        QMutexLocker lock(&m_plane->mutex);
        m_plane->outbound.abortActive();
        reportQueueDropsLocked();
    }
    notifyUploadDropped(cause, 1);
    QMetaObject::invokeMethod(this, [this] { drainOutbound(); }, Qt::QueuedConnection);
}

// Reports queue-side drops once each: expired entries and dependents retracted by
// the dependency rule. Called with the data-plane mutex held.
void NetworkEngine::reportQueueDropsLocked()
{
    if (!m_plane) return;
    const auto expired = m_plane->outbound.expired() - m_lastExpired;
    if (expired > 0) {
        m_lastExpired += expired;
        m_stats.uploadPacketsDroppedAge += expired;
        notifyUploadDropped(UploadDropCause::QueueAge, int(expired));
    }
    const auto dependency =
        m_plane->outbound.dependencyDropped() - m_lastDependencyDropped;
    if (dependency > 0) {
        m_lastDependencyDropped += dependency;
        m_stats.uploadPacketsDroppedDependency += dependency;
        notifyUploadDropped(UploadDropCause::Dependency, int(dependency));
    }
}

void NetworkEngine::tick()
{
    if (m_stopped) return;
    const qint64 now = nowUs();
    const qint64 ms = now / 1000;
    if (m_connecting && now >= m_connectDeadlineUs) {
        if (m_tcpConnected) {
            failConnect(QStringLiteral("已建立 TCP 连接，但 5 秒内未完成桌面共享握手（未收到 hello 响应）"));
        } else {
            failConnect(QStringLiteral("连接服务端超时（5 秒内未建立 TCP 连接）"));
        }
        return;
    }
    if (m_connected && now >= m_nextPingUs) {
        m_nextPingUs = now + KeepaliveUs;
        sendRequest(Operation::Ping, [](Request &) {});
    }
    // Expired control requests must not leave their operations behind. A
    // pending hello is part of the connection attempt, so its deadline uses the
    // same unified failure cleanup instead of a generic control error that
    // would leave the UI in connecting.
    if (!m_operations.isEmpty()) {
        for (auto it = m_operations.begin(); it != m_operations.end();) {
            if (!m_pending.contains(it.key(), ms)) {
                const auto operation = it.value();
                const auto name = operationName(operation);
                it = m_operations.erase(it);
                ++m_stats.controlErrors;
                if (operation == Operation::Hello && !m_connected) {
                    failConnect(QStringLiteral("等待服务端桌面共享握手响应超时"));
                    return;
                }
                notifyGui(GuiNotice::ControlFailed,
                          [name](GuiNoticeSlot &slot) {
                              mergeControlFailure(slot, name, int(ErrorCode::Timeout), QStringLiteral("控制请求超时"));
                          });
            } else {
                ++it;
            }
        }
        m_pending.expire(ms);
    }
    if (m_inRoom) {
        if (!m_registered && !m_registrationFailed) {
            if (now >= m_nextRegisterUs) {
                if (m_registerAttempts >= MaxRegisterAttempts) {
                    m_registrationFailed = true;
                    notifyUdpReady(false, QStringLiteral("UDP 注册超时"));
                } else {
                    sendRegister(false);
                }
            }
        } else if (m_registered) {
            // Both directions of live media prove the endpoint is alive:
            // a publisher's continuing upload and a viewer's continuing inbound
            // datagrams. Registration acknowledgements can be delayed by the
            // bounded host UDP budget during heavy 1080p fan-out even though the
            // media path itself is working.
            m_endpointActivity.observe(now, m_stats.datagramsSent, m_stats.inboundDatagrams);
            if (m_endpointActivity.expired(now, UdpIdleUs)) {
                m_registered = false;
                m_registerAttempts = 0;
                m_nextRegisterUs = now;
                notifyUdpReady(false, QStringLiteral("UDP 保活超时"));
            } else if (now >= m_nextRegisterUs) {
                sendRegister(true);
            }
        }
    }
    if (m_receiver.configured()) {
        QList<VideoPacket> completed;
        const bool gap = m_receiver.tick(now, completed);
        for (auto &packet : completed) deliverInbound(std::move(packet));
        if (gap) {
            notifyGui(GuiNotice::StreamGap, [](GuiNoticeSlot &) {});
        }
        if (m_inboundRecovery && now >= m_nextMailboxRecoveryUs) {
            m_nextMailboxRecoveryUs = now + 1000000;
            notifyGui(GuiNotice::StreamGap, [](GuiNoticeSlot &) {});
        }
    }
    if (m_plane) {
        QMutexLocker lock(&m_plane->mutex);
        // Age and dependency invalidation are handled on the network thread even
        // when no push/acquire happens, so a stalled drain cannot leave queued
        // packets whose key frame already expired.
        m_plane->outbound.tick(now);
        reportQueueDropsLocked();
        const bool pending = m_plane->outbound.count() > 0;
        lock.unlock();
        if (pending && !m_active) drainOutbound();
    }
    publishStats();
}

// ---------------------------------------------------------------------------
// GUI-thread facade

NetworkClient::NetworkClient(QObject *parent)
    : QObject(parent), m_plane(std::make_shared<DataPlane>())
{
}

NetworkClient::~NetworkClient()
{
    // The stop contract retains the owner on failure. Destruction outside
    // that contract still cannot destroy a running QThread or its facade.
    shutdown(-1);
}

void NetworkClient::start()
{
    if (m_thread) return;
    m_thread = new QThread(this);
    m_engine = new NetworkEngine(m_plane, this);
    m_engine->moveToThread(m_thread);
    connect(m_thread, &QThread::started, m_engine, [this] { m_engine->begin(); });
    connect(m_thread, &QThread::finished, m_engine, &QObject::deleteLater);
    m_running.store(true);
    m_accepting.store(true);
    m_thread->start();
}

void NetworkClient::beginShutdown()
{
    if (!m_thread || m_stopQueued) return;
    m_stopQueued = true;
    m_accepting.store(false);
    {
        QMutexLocker lock(&m_plane->mutex);
        m_plane->closed = true;
        m_plane->outbound.close();
        m_plane->inbound.close();
    }
    // A stopped client delivers nothing more, so the bounded board is emptied
    // here instead of being drained later: an unconsumed UDP failure of the
    // session that is being stopped must not surface as state of a stopped or
    // subsequent session. The lifetime advances first, so notices a drain that is
    // running on this thread already copied are re-validated as stale and are not
    // emitted after the stop either.
    {
        QMutexLocker lock(&m_plane->notices.mutex);
        auto &board = m_plane->notices;
        advanceNoticeLifetimeLocked(board);
        for (int index = 0; index < GuiNoticeCount; ++index) {
            auto &slot = board.entries[index];
            if (!slot.pending) continue;
            slot.pending = false;
            --board.pending;
            ++board.stale;
        }
        for (int index = 0; index < UploadDropCauseCount; ++index) board.dropCauses[index] = 0;
        board.wake.store(false);
    }
    if (m_thread->isRunning() && m_engine) {
        QMetaObject::invokeMethod(m_engine, [engine = m_engine] { engine->stop(); },
                                  Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_engine, [] { QThread::currentThread()->quit(); },
                                  Qt::QueuedConnection);
    }
}

void NetworkClient::resetNoticeLifetime()
{
    if (!m_plane) return;
    QMutexLocker lock(&m_plane->notices.mutex);
    advanceNoticeLifetimeLocked(m_plane->notices);
}

bool NetworkClient::shutdown(int timeoutMs)
{
    QDeadlineTimer deadline(timeoutMs);
    beginShutdown();
    if (m_thread && !m_thread->wait(deadline)) return false;
    m_running.store(false);
    return true;
}

void NetworkClient::shutdownAsync(std::function<void(bool)> completed, int timeoutMs)
{
    const QDeadlineTimer deadline(timeoutMs);
    beginShutdown();
    auto *poll = new QTimer(this);
    poll->setInterval(1);
    connect(poll, &QTimer::timeout, this, [this, poll, deadline, completed = std::move(completed)] {
        const bool stopped = !m_thread || m_thread->wait(0);
        if (!stopped && !deadline.hasExpired()) return;
        poll->stop();
        poll->deleteLater();
        if (stopped) m_running.store(false);
        completed(stopped);
    });
    poll->start();
}

bool NetworkClient::enqueueVideo(const VideoPacket &packet)
{
    if (!m_running.load() || !m_plane) return false;
    const qint64 now = nowUs();
    bool accepted = false;
    {
        QMutexLocker lock(&m_plane->mutex);
        if (m_plane->closed) return false;
        accepted = m_plane->outbound.push(packet, now);
        if (accepted) {
            m_plane->outboundPeakPackets = std::max(m_plane->outboundPeakPackets,
                                                    m_plane->outbound.count());
            m_plane->outboundPeakBytes = std::max(m_plane->outboundPeakBytes,
                                                  m_plane->outbound.bytes());
        } else {
            ++m_plane->uploadQueueRejected;
        }
    }
    if (!accepted) return false;
    if (!m_plane->wakeOutbound.exchange(true) && m_engine) {
        QMetaObject::invokeMethod(m_engine, [engine = m_engine] { engine->drainOutbound(); },
                                  Qt::QueuedConnection);
    }
    return true;
}

int NetworkClient::takeVideo(QList<VideoPacket> &out)
{
    QMutexLocker lock(&m_plane->mutex);
    return m_plane->inbound.take(nowUs(), out);
}

void NetworkClient::drainInbound()
{
    if (!m_accepting.load()) return;
    m_plane->wakeInbound.store(false);
    bool pending = false;
    {
        QMutexLocker lock(&m_plane->mutex);
        pending = m_plane->inbound.count() > 0;
    }
    if (pending) emit incomingVideo();
}

NetworkStats NetworkClient::stats() const
{
    NetworkStats snapshot;
    {
        QMutexLocker lock(&m_plane->mutex);
        snapshot = m_plane->snapshot;
        snapshot.outboundQueued = m_plane->outbound.count();
        snapshot.outboundBytes = m_plane->outbound.bytes();
        snapshot.inboundQueued = m_plane->inbound.count();
        snapshot.inboundBytes = m_plane->inbound.bytes();
        snapshot.outboundPeakBytes = m_plane->outboundPeakBytes;
        snapshot.outboundPeakPackets = m_plane->outboundPeakPackets;
        snapshot.inboundPeakBytes = m_plane->inboundPeakBytes;
        snapshot.inboundPeakPackets = m_plane->inboundPeakPackets;
        snapshot.uploadPacketsDroppedQueue = m_plane->uploadQueueRejected;
    }
    // Separate lock: the board is posted to while the data-plane mutex is held,
    // so it must never be read inside that critical section.
    {
        QMutexLocker lock(&m_plane->notices.mutex);
        snapshot.guiPendingNotices = m_plane->notices.pending;
        snapshot.guiPeakNotices = m_plane->notices.peak;
        snapshot.guiNoticesCoalesced = m_plane->notices.coalesced;
        snapshot.guiNoticesDelivered = m_plane->notices.delivered;
        snapshot.guiNoticesStale = m_plane->notices.stale;
    }
    return snapshot;
}

// GUI thread: emits the merged notifications that accumulated since the last
// drain, in the order their kinds first became pending. The network thread posts
// this entry point at most once at a time, so a long GUI stall keeps a fixed
// number of pending tasks and the payloads carry the cumulative counts.
//
// Emitting a notice runs controller callbacks, and one of them may stop the room,
// open or join another one, disconnect or stop the client while this same drain
// still holds payloads it copied under the lock. Every copied notice is therefore
// re-validated against the live room/media lifetime immediately before it is
// emitted, and the UDP slot is re-validated between its failure and its recovered
// state. A stale copy is counted in `guiNoticesStale` and dropped instead of being
// replayed into the session that exists afterwards. The board lock is never held
// while a signal is emitted.
void NetworkClient::drainNotices()
{
    if (!m_plane || !m_accepting.load()) return;
    struct Pending {
        GuiNotice kind;
        GuiNoticeSlot slot;
    };
    QList<Pending> pending;
    quint64 causes[UploadDropCauseCount] = {};
    {
        QMutexLocker lock(&m_plane->notices.mutex);
        auto &board = m_plane->notices;
        for (int index = 0; index < GuiNoticeCount; ++index) {
            auto &slot = board.entries[index];
            if (!slot.pending) continue;
            pending.append(Pending{GuiNotice(index), slot});
            // Control failures belong only to this drain's batch. Detach them
            // under the board lock so failures posted during callbacks form a
            // new batch, including a new failure of the same operation.
            if (GuiNotice(index) == GuiNotice::ControlFailed) slot.failures.clear();
            slot.pending = false;
            --board.pending;
        }
        for (int index = 0; index < UploadDropCauseCount; ++index) {
            causes[index] = board.dropCauses[index];
            board.dropCauses[index] = 0;
        }
        board.wake.store(false);
    }
    std::sort(pending.begin(), pending.end(), [](const Pending &left, const Pending &right) {
        return left.slot.sequence < right.slot.sequence;
    });
    const auto liveLifetime = [this] {
        QMutexLocker lock(&m_plane->notices.mutex);
        return m_plane->notices.udpGeneration;
    };
    const auto countStale = [this] {
        QMutexLocker lock(&m_plane->notices.mutex);
        ++m_plane->notices.stale;
    };
    const auto countDelivered = [this] {
        QMutexLocker lock(&m_plane->notices.mutex);
        ++m_plane->notices.delivered;
    };
    const auto isStale = [this, &liveLifetime](const Pending &entry) {
        // A stop empties the board and invalidates everything that is still being
        // drained; a room lifetime change invalidates only the notices that
        // describe it.
        if (!m_accepting.load()) return true;
        if (!noticeDescribesLifetime(entry.kind)) return false;
        return entry.slot.generation != liveLifetime();
    };
    for (const auto &entry : pending) {
        const auto &slot = entry.slot;
        if (isStale(entry)) {
            countStale();
            continue;
        }
        switch (entry.kind) {
        case GuiNotice::Connected:
            emit connected(slot.first, quint16(slot.second));
            break;
        case GuiNotice::ConnectFailed:
            emit connectFailed(slot.text);
            break;
        case GuiNotice::ConnectionLost:
            emit connectionLost(slot.text);
            break;
        case GuiNotice::ControlFailed:
            // At most MaxPendingControlFailures distinct operations, each emitted
            // once with its latest error.
            for (const auto &failure : slot.failures)
                emit controlFailed(failure.operation, failure.code, failure.message);
            break;
        case GuiNotice::RoomCreated:
            emit roomCreated(slot.room.roomId, slot.room.roomCode, slot.room.profile,
                             slot.room.token, slot.room.hasFormat, slot.room.format);
            break;
        case GuiNotice::RoomJoined:
            emit roomJoined(slot.room.roomId, slot.room.roomCode, slot.room.profile,
                            slot.room.token, slot.room.hasFormat, slot.room.format);
            break;
        case GuiNotice::RoomStateChanged:
            emit roomStateChanged(slot.first, slot.code, int(slot.value));
            break;
        case GuiNotice::StreamConfig:
            emit streamConfigReceived(slot.first, slot.format);
            break;
        case GuiNotice::StreamKeyframe:
            emit streamKeyframeRequested(slot.first, slot.second);
            break;
        case GuiNotice::RoomClosed:
            emit roomClosed(slot.first, slot.code);
            break;
        case GuiNotice::StreamConfigured:
            emit streamConfigured(slot.first);
            break;
        case GuiNotice::StreamReadyConfirmed:
            emit streamReadyConfirmed(slot.first);
            break;
        case GuiNotice::UdpReady:
            // Failure first, state second: a controller that acted on the loss
            // still hears about it even though the endpoint recovered before the
            // GUI drained the board. The failure handler itself may close the
            // room or stop the client, so the recovered state of the old lifetime
            // is re-validated and is not delivered after such a transition.
            if (slot.failurePending) emit udpReady(false, slot.text);
            if (slot.flag) {
                if (isStale(entry)) countStale();
                else emit udpReady(true, QString());
            }
            break;
        case GuiNotice::UploadDropped: {
            quint64 total = 0;
            QStringList reasons;
            for (int index = 0; index < UploadDropCauseCount; ++index) {
                if (causes[index] == 0) continue;
                total += causes[index];
                reasons.append(QStringLiteral("%1 %2")
                                   .arg(uploadDropCauseName(UploadDropCause(index)))
                                   .arg(causes[index]));
            }
            if (total > 0) emit uploadDropped(int(total), reasons.join(QStringLiteral("，")));
            break;
        }
        case GuiNotice::FeedbackCompleted:
            emit feedbackCompleted(slot.flag, slot.code);
            break;
        case GuiNotice::StreamGap:
            emit streamGapDetected();
            break;
        case GuiNotice::Count:
            break;
        }
        countDelivered();
    }
}

QString NetworkClient::uplinkDescription() const
{
    QMutexLocker lock(&m_plane->mutex);
    return QStringLiteral("single server uplink (%1 datagrams sent, %2 dropped)")
        .arg(m_plane->snapshot.datagramsSent)
        .arg(m_plane->snapshot.datagramsDropped);
}


void NetworkClient::connectToServer(const QString &address, quint16 port)
{
    const auto target = m_engine;
    if (!m_accepting.load() || !target) return;
    resetNoticeLifetime();
    QMetaObject::invokeMethod(target,
                              [engine = target, address, port] { engine->connectTo(address, port); },
                              Qt::QueuedConnection);
}

void NetworkClient::disconnectFromServer(const QString &reason)
{
    const auto target = m_engine;
    if (!m_accepting.load() || !target) return;
    resetNoticeLifetime();
    QMetaObject::invokeMethod(target,
                              [engine = target, reason] { engine->disconnectFrom(reason); },
                              Qt::QueuedConnection);
}

void NetworkClient::createRoom(const QString &profile)
{
    const auto target = m_engine;
    if (!m_accepting.load() || !target) return;
    resetNoticeLifetime();
    QMetaObject::invokeMethod(target, [engine = target, profile] { engine->create(profile); },
                              Qt::QueuedConnection);
}

void NetworkClient::joinRoom(const QString &code)
{
    const auto target = m_engine;
    if (!m_accepting.load() || !target) return;
    resetNoticeLifetime();
    QMetaObject::invokeMethod(target, [engine = target, code] { engine->join(code); },
                              Qt::QueuedConnection);
}

void NetworkClient::leaveRoom(const QString &reason)
{
    const auto target = m_engine;
    if (!m_accepting.load() || !target) return;
    resetNoticeLifetime();
    QMetaObject::invokeMethod(target, [engine = target, reason] { engine->leave(reason); },
                              Qt::QueuedConnection);
}

void NetworkClient::stopRoom(const QString &reason)
{
    const auto target = m_engine;
    if (!m_accepting.load() || !target) return;
    resetNoticeLifetime();
    QMetaObject::invokeMethod(target, [engine = target, reason] { engine->stopRoom(reason); },
                              Qt::QueuedConnection);
}

void NetworkClient::configureStream(const StreamFormat &format)
{
    const auto target = m_engine;
    if (!m_accepting.load() || !target) return;
    QMetaObject::invokeMethod(target, [engine = target, format] { engine->configure(format); },
                              Qt::QueuedConnection);
}

void NetworkClient::streamReady(quint64 epoch)
{
    const auto target = m_engine;
    if (!m_accepting.load() || !target) return;
    QMetaObject::invokeMethod(target, [engine = target, epoch] { engine->ready(epoch); },
                              Qt::QueuedConnection);
}

void NetworkClient::requestKeyframe(quint64 epoch)
{
    const auto target = m_engine;
    if (!m_accepting.load() || !target) return;
    QMetaObject::invokeMethod(target, [engine = target, epoch] { engine->keyframe(epoch); },
                              Qt::QueuedConnection);
}

void NetworkClient::sendFeedback(const Feedback &values, quint64 epoch)
{
    const auto target = m_engine;
    if (!m_accepting.load() || !target) return;
    QMetaObject::invokeMethod(target,
                              [engine = target, values, epoch] { engine->feedback(values, epoch); },
                              Qt::QueuedConnection);
}

void NetworkClient::beginReceive(quint64 epoch)
{
    const auto target = m_engine;
    if (!m_accepting.load() || !target) return;
    QMetaObject::invokeMethod(target, [engine = target, epoch] { engine->beginReceive(epoch); },
                              Qt::QueuedConnection);
}

void NetworkClient::resyncReceive()
{
    const auto target = m_engine;
    if (!m_accepting.load() || !target) return;
    QMetaObject::invokeMethod(target, [engine = target] { engine->resyncReceive(); },
                              Qt::QueuedConnection);
}

void NetworkClient::clearReceive()
{
    const auto target = m_engine;
    if (!m_accepting.load() || !target) return;
    QMetaObject::invokeMethod(target, [engine = target] { engine->clearReceive(); },
                              Qt::QueuedConnection);
}

void NetworkClient::clearUpload()
{
    const auto target = m_engine;
    if (!m_accepting.load() || !target) return;
    QMetaObject::invokeMethod(target, [engine = target] { engine->clearUpload(); },
                              Qt::QueuedConnection);
}


} // namespace vusiif::zto::desktop_share
