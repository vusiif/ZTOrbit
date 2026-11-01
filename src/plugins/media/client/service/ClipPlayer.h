#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include <functional>

namespace vusiif::zto::media::engine {

// Drives one short clip from the accepted request to exactly one terminal
// outcome: finished() after the device mix queue and the sink backlog drained,
// or failed() when feeding, the output device or the drain deadline fails.
// stop() is a silent cancellation and emits nothing.
//
// The owner supplies the device-thread work through hooks. Implementations must
// invoke the supplied completion on this object's thread (post it if needed);
// ClipPlayer guards every pending callback with a QPointer so erasing the
// player during teardown is safe.
class ClipPlayer final : public QObject {
    Q_OBJECT
public:
    struct DrainState {
        int queuedFrames = 0;
        qint64 pendingBytes = 0;
        bool output = true;
        bool sinkActive = true;
    };
    struct Hooks {
        // Feeds one 20 ms chunk and reports whether the device accepted it.
        std::function<void(QVector<float> chunk, std::function<void(bool)> completed)> feed;
        // Reports the per-track mix queue and the not-yet-written sink backlog.
        std::function<void(std::function<void(const DrainState &)> completed)> query;
        std::function<void()> finished;
        std::function<void(const QString &message)> failed;
    };

    ClipPlayer(QString id, QString lease, QVector<QVector<float>> chunks, Hooks hooks,
               QObject *parent = nullptr);
    ~ClipPlayer() override;

    const QString &id() const { return m_id; }
    const QString &lease() const { return m_lease; }
    void start();
    // Silent cancellation: no finished/failed event reaches the consumer.
    void stop();
    bool stopped() const { return m_stopped; }
    bool terminal() const { return m_done; }
    // Test hook: bounds how long the tail may stay in the output queue.
    void setDrainTimeoutMs(int milliseconds);
    // Test hook: bounds the whole clip even if a device-thread reply is lost.
    void setWatchdogTimeoutMs(int milliseconds);

private:
    void tick();
    void feedNext();
    void fed(bool accepted);
    void checkDrain();
    void drained(const DrainState &state);
    void finish();
    void fail(const QString &message);

    QString m_id;
    QString m_lease;
    QVector<QVector<float>> m_chunks;
    Hooks m_hooks;
    QTimer m_timer;
    QElapsedTimer m_clock;
    int m_next = 0;
    int m_drainTimeoutMs = 5000;
    int m_watchdogTimeoutMs = 10000;
    bool m_started = false;
    bool m_stopped = false;
    bool m_done = false;
    bool m_waitingFeed = false;
    bool m_waitingQuery = false;
    qint64 m_drainDeadline = 0;
    qint64 m_watchdogDeadline = 0;
};

} // namespace vusiif::zto::media::engine
