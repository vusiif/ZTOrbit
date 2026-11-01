#include "ClipPlayer.h"

#include <QPointer>

#include <utility>

namespace vusiif::zto::media::engine {

ClipPlayer::ClipPlayer(QString id, QString lease, QVector<QVector<float>> chunks, Hooks hooks,
                       QObject *parent)
    : QObject(parent), m_id(std::move(id)), m_lease(std::move(lease)), m_chunks(std::move(chunks)),
      m_hooks(std::move(hooks))
{
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(20);
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
}

ClipPlayer::~ClipPlayer() { m_timer.stop(); }

void ClipPlayer::setDrainTimeoutMs(int milliseconds)
{
    m_drainTimeoutMs = qMax(0, milliseconds);
}

void ClipPlayer::setWatchdogTimeoutMs(int milliseconds)
{
    m_watchdogTimeoutMs = qMax(0, milliseconds);
}

void ClipPlayer::start()
{
    if (m_started || m_done || m_stopped) return;
    m_started = true;
    m_clock.start();
    m_watchdogDeadline = m_clock.elapsed() + m_watchdogTimeoutMs
        + qint64(m_chunks.size()) * 20;
    if (!m_hooks.feed || !m_hooks.query) {
        fail(QStringLiteral("音频播放钩子不可用"));
        return;
    }
    feedNext();
    if (!m_done && !m_stopped) m_timer.start();
}

void ClipPlayer::stop()
{
    m_stopped = true;
    m_timer.stop();
}

void ClipPlayer::tick()
{
    if (m_done || m_stopped) return;
    if (m_clock.elapsed() > m_watchdogDeadline) {
        fail(QStringLiteral("音频播放超时"));
        return;
    }
    if (m_next < m_chunks.size()) {
        if (!m_waitingFeed) feedNext();
        return;
    }
    checkDrain();
}

void ClipPlayer::feedNext()
{
    if (m_done || m_stopped || m_waitingFeed || m_next >= m_chunks.size()) return;
    m_waitingFeed = true;
    const QPointer<ClipPlayer> self(this);
    m_hooks.feed(m_chunks.at(m_next), [self](bool accepted) {
        if (self) self->fed(accepted);
    });
}

void ClipPlayer::fed(bool accepted)
{
    m_waitingFeed = false;
    if (m_done || m_stopped) return;
    if (!accepted) {
        fail(QStringLiteral("音频输出未能接受播放数据"));
        return;
    }
    ++m_next;
    if (m_next >= m_chunks.size())
        m_drainDeadline = m_clock.elapsed() + m_drainTimeoutMs;
}

void ClipPlayer::checkDrain()
{
    if (m_done || m_stopped || m_waitingQuery) return;
    m_waitingQuery = true;
    const QPointer<ClipPlayer> self(this);
    m_hooks.query([self](const DrainState &state) {
        if (self) self->drained(state);
    });
}

void ClipPlayer::drained(const DrainState &state)
{
    m_waitingQuery = false;
    if (m_done || m_stopped) return;
    if (!state.output || !state.sinkActive) {
        fail(QStringLiteral("音频输出设备不可用"));
        return;
    }
    if (state.queuedFrames == 0 && state.pendingBytes == 0) {
        finish();
        return;
    }
    if (m_clock.elapsed() > m_drainDeadline)
        fail(QStringLiteral("音频输出未在限定时间内排空"));
}

void ClipPlayer::finish()
{
    if (m_done || m_stopped) return;
    m_done = true;
    m_timer.stop();
    if (m_hooks.finished) m_hooks.finished();
}

void ClipPlayer::fail(const QString &message)
{
    if (m_done || m_stopped) return;
    m_done = true;
    m_timer.stop();
    if (m_hooks.failed) m_hooks.failed(message);
}

} // namespace vusiif::zto::media::engine
