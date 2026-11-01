#pragma once

#include <desktop-share/ControlProtocol.h>
#include <desktop-share/MediaProtocol.h>
#include <desktop-share/StreamReceiver.h>

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <atomic>
#include <memory>
#include <functional>

class QThread;

namespace vusiif::zto::desktop_share {

struct NetworkStats {
    QString localAddress;
    quint16 localUdpPort = 0;
    quint16 serverUdpPort = 0;
    quint64 mediaStreamEpoch = 0;
    quint64 tcpRequestsSent = 0;
    quint64 tcpResponsesReceived = 0;
    quint64 controlErrors = 0;
    quint64 lateResponses = 0;
    quint64 datagramsSent = 0;
    quint64 datagramsDropped = 0;
    quint64 uploadPackets = 0;
    quint64 uploadPacketsDroppedBudget = 0;
    quint64 uploadPacketsDroppedAge = 0;
    quint64 uploadPacketsDroppedQueue = 0;
    // The packet's whole wire cost exceeded the 1 MiB pacing burst, so it could
    // never finish inside the receiver's 200 ms reassembly window.
    quint64 uploadPacketsDroppedPacing = 0;
    // An admitted packet could not finish inside that window (event-loop or
    // socket stall) and was aborted instead of dripping unusable fragments.
    quint64 uploadPacketsDroppedDeadline = 0;
    // Queued or future packets retracted because a packet they depend on was
    // lost locally; the wire never receives them.
    quint64 uploadPacketsDroppedDependency = 0;
    // Packets that waited for their whole-packet send budget before starting.
    quint64 uploadPacketsDeferred = 0;
    quint64 pacingWaits = 0;
    quint64 publishRateBytesPerSecond = 0;
    // Tokens currently available to the publisher (never more than the 1 MiB
    // burst). Exposed for congestion evidence; it holds no media content.
    qint64 publishBudgetBytes = 0;
    // Bounded GUI notification board: the network thread merges every
    // notification into a fixed slot table and posts at most one wakeup, so a
    // stalled GUI thread keeps a constant pending-task count.
    int guiPendingNotices = 0;
    int guiPeakNotices = 0;
    quint64 guiNoticesCoalesced = 0;
    quint64 guiNoticesDelivered = 0;
    // Notices discarded because they belonged to a room/media lifetime that was
    // already closed, so they can never be replayed into a newer session.
    quint64 guiNoticesStale = 0;
    quint64 inboundDatagrams = 0;
    quint64 inboundRejected = 0;
    quint64 inboundDroppedBudget = 0;
    quint64 inboundDelivered = 0;
    quint64 udpRegistersSent = 0;
    quint64 udpAckReceived = 0;
    quint64 keepalivesSent = 0;
    int outboundQueued = 0;
    int inboundQueued = 0;
    qint64 outboundBytes = 0;
    qint64 inboundBytes = 0;
    qint64 outboundPeakBytes = 0;
    qint64 inboundPeakBytes = 0;
    int outboundPeakPackets = 0;
    int inboundPeakPackets = 0;
    StreamReceiver::Stats receiver;
};

// Client side of the desktop-share transport. It owns a dedicated network
// thread that keeps the TCP control socket, the UDP media socket, the pending
// request table, fragment splitting and the reassembly/ordered-commit state.
// Nothing in this class touches media sessions or the GUI: completed packets
// leave through a bounded mailbox (four packets / 4 MiB) and the GUI pulls them
// after one coalesced wakeup. Video entering the network thread is budgeted
// before it is queued and is discarded instead of growing an unbounded event
// backlog. Publisher uploads are paced to at most profileBitrate x 1.25 with a
// 1 MiB burst, and one encoded packet is admitted as a whole: it starts only
// when its complete wire cost is available, so it can finish inside the
// receiver's 200 ms reassembly window, and it is dropped together with its
// queued dependents when that is impossible. A deferred admission never holds the
// send slot in front of a newer key frame. The per-turn fragment batch adapts to
// the observed event-loop turn interval, so a host with a coarse timer tick or a
// slow turn still finishes a legal packet inside the same active-send deadline.
//
// Control and drop notifications to the GUI also go through one bounded board:
// each notification kind has a fixed slot (newest payload, accumulated count)
// and the network thread posts at most one wakeup, so a stalled GUI thread keeps
// a constant number of pending tasks instead of one per lost packet.
class NetworkClient final : public QObject {
    Q_OBJECT

public:
    explicit NetworkClient(QObject *parent = nullptr);
    ~NetworkClient() override;

    void start();
    // Stops delivery, closes the sockets and waits for the network thread. A
    // false result means the thread did not exit inside the timeout; the
    // caller must not unload the plugin in that case.
    bool shutdown(int timeoutMs = 5000);
    void shutdownAsync(std::function<void(bool)> completed, int timeoutMs = 5000);
    bool running() const { return m_running.load(); }

    // Thread-safe video data plane. A false result means the bounded queue
    // rejected the packet and the caller must count the drop.
    bool enqueueVideo(const VideoPacket &packet);
    // GUI thread: moves completed packets out of the bounded inbox.
    int takeVideo(QList<VideoPacket> &out);

    // Control commands. All are forwarded to the network thread; a command
    // issued after shutdown() is ignored.
    void connectToServer(const QString &address, quint16 port);
    void disconnectFromServer(const QString &reason = QString());
    void createRoom(const QString &profile);
    void joinRoom(const QString &code);
    void leaveRoom(const QString &reason = QString());
    void stopRoom(const QString &reason = QString());
    void configureStream(const StreamFormat &format);
    void streamReady(quint64 epoch);
    void requestKeyframe(quint64 epoch);
    void sendFeedback(const Feedback &feedback, quint64 epoch);
    // Arms the ordered receiver for a stream epoch and drops the previous
    // epoch's reassembly state.
    void beginReceive(quint64 epoch);
    void resyncReceive();
    void clearReceive();
    // Drops queued upload packets during a source/profile switch.
    void clearUpload();

    NetworkStats stats() const;
    // Test/diagnostic hook: the single uplink destination currently in use.
    QString uplinkDescription() const;

signals:
    void connected(quint64 sessionId, quint16 udpPort);
    void connectFailed(const QString &message);
    void connectionLost(const QString &message);
    void roomCreated(quint64 roomId, const QString &code, const QString &profile,
                     const QByteArray &token, bool hasFormat, const StreamFormat &format);
    void roomJoined(quint64 roomId, const QString &code, const QString &profile,
                    const QByteArray &token, bool hasFormat, const StreamFormat &format);
    void roomStateChanged(quint64 roomId, int state, int viewers);
    void streamConfigReceived(quint64 roomId, const StreamFormat &format);
    void streamKeyframeRequested(quint64 roomId, quint64 epoch);
    void roomClosed(quint64 roomId, int reason);
    void controlFailed(const QString &operation, int errorCode, const QString &message);
    void udpReady(bool ok, const QString &message);
    void streamConfigured(quint64 epoch);
    void streamReadyConfirmed(quint64 epoch);
    // One feedback round finished: `ok` is false for a server rejection and
    // `errorCode` is then the decoded error. The controller paces the next
    // report from the acknowledgement so the server's one-second window is
    // measured from its own processing time, not from the client's send time.
    void feedbackCompleted(bool ok, int errorCode);
    void uploadDropped(int packets, const QString &reason);
    void streamGapDetected();
    void incomingVideo();

private:
    friend class NetworkEngine;

    std::shared_ptr<class DataPlane> m_plane;
    class NetworkEngine *m_engine = nullptr;
    QThread *m_thread = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_accepting{false};
    bool m_stopQueued = false;
    void beginShutdown();
    void drainInbound();
    // GUI thread: a lifecycle command issued by the caller starts a new session
    // lifetime for queued notices, so a notice already copied by a running drain
    // is re-validated as stale before it is emitted. It only advances the bounded
    // board's generation; it does not queue anything or wait for the network
    // thread.
    void resetNoticeLifetime();
    // GUI thread: drains the bounded notification board. The network thread only
    // ever posts this one entry point.
    void drainNotices();
};

} // namespace vusiif::zto::desktop_share
