#include "VoiceSoundPlayer.h"

#include <algorithm>

namespace vusiif::zto::voice_chat {
namespace {
constexpr qint64 MaxWaitingMs = 2000;
constexpr int MaxItems = 8;
}
VoiceSoundPlayer::VoiceSoundPlayer(std::shared_ptr<media::AudioClipClient> client,
                                   QByteArray joinWav, QByteArray leaveWav, QObject *parent)
    : QObject(parent), m_client(std::move(client)), m_join(std::move(joinWav)),
      m_leave(std::move(leaveWav))
{
    m_clock.start();
    if (!m_client) return;
    m_client->finished = [this](const QString &clipId) { finish(clipId); };
    m_client->failed = [this](const QString &clipId, const media::MediaError &) { fail(clipId); };
}

VoiceSoundPlayer::~VoiceSoundPlayer() { stop(); }

void VoiceSoundPlayer::setOutputDevice(const QByteArray &device)
{
    if (device == m_device) return;
    m_device = device;
    if (!m_current.isEmpty() && m_client) {
        m_client->stop(m_current, this, {});
        m_current.clear();
        ++m_playSerial;
    }
    playNext();
}

void VoiceSoundPlayer::setVolume(int percent) { m_volume = std::clamp(percent, 0, 200); }

void VoiceSoundPlayer::setDeafened(bool deafened)
{
    if (m_deafened == deafened) return;
    m_deafened = deafened;
    if (m_deafened) {
        if (!m_current.isEmpty() && m_client) m_client->stop(m_current, this, {});
        m_current.clear();
        ++m_playSerial;
        m_waiting.clear();
    }
}

void VoiceSoundPlayer::prune()
{
    const auto now = m_clock.elapsed();
    while (!m_waiting.isEmpty() && now - m_waiting.front().queuedAt > MaxWaitingMs)
        m_waiting.dequeue();
}

void VoiceSoundPlayer::enqueue(Cue cue)
{
    if (m_stopping || m_deafened || !m_client) return;
    prune();
    // Merge only adjacent cues of the same kind. A cue arriving while an
    // identical cue is playing is part of the same burst and is merged too.
    if (!m_waiting.isEmpty() && m_waiting.last().cue == cue) return;
    if (m_waiting.isEmpty() && !m_current.isEmpty() && m_currentCue == cue) return;
    if (m_waiting.size() + (m_current.isEmpty() ? 0 : 1) >= MaxItems) {
        if (m_waiting.isEmpty()) return;
        m_waiting.dequeue();
    }
    m_waiting.enqueue({cue, m_clock.elapsed()});
    playNext();
}

void VoiceSoundPlayer::playNext()
{
    if (m_stopping || m_deafened || !m_client || !m_current.isEmpty()) return;
    prune();
    while (!m_waiting.isEmpty()) {
        const auto item = m_waiting.dequeue();
        const auto &bytes = item.cue == Cue::Join ? m_join : m_leave;
        if (bytes.isEmpty()) continue;
        m_currentCue = item.cue;
        m_current = QStringLiteral("starting");
        const auto gain = m_volume / 100.0;
        // A terminal event may advance the queue before the asynchronous play
        // reply arrives; the serial makes the stale reply harmless.
        const int serial = ++m_playSerial;
        m_client->play(QString::fromLatin1(m_device.toHex()), gain, bytes, this,
                       [this, serial](media::Result<QString> result) {
                           if (serial != m_playSerial) return;
                           if (!result.ok() || result.value.isEmpty()) {
                               m_current.clear();
                               ++m_playSerial;
                               playNext();
                               return;
                           }
                           if (m_earlyFinished.remove(result.value)) {
                               m_current.clear();
                               ++m_playSerial;
                               playNext();
                               return;
                           }
                           if (m_current == QLatin1String("starting")) m_current = result.value;
                       });
        return;
    }
}

void VoiceSoundPlayer::finish(const QString &clipId)
{
    if (m_current.isEmpty()) return;
    if (m_current == QLatin1String("starting")) {
        if (clipId.isEmpty()) {
            // A terminal event without a usable ID still belongs to the pending
            // request; never leave the queue stuck on "starting".
            m_current.clear();
            ++m_playSerial;
            playNext();
            return;
        }
        m_earlyFinished.insert(clipId);
        return;
    }
    if (m_current != clipId) return;
    m_current.clear();
    ++m_playSerial;
    playNext();
}

void VoiceSoundPlayer::fail(const QString &clipId)
{
    if (m_current.isEmpty()) return;
    // Failure advances the queue for the current request. The worker reports
    // failed at most once per clip, so an unknown/empty ID is still terminal.
    if (m_current == QLatin1String("starting")) {
        m_current.clear();
        m_earlyFinished.remove(clipId);
        ++m_playSerial;
        playNext();
        return;
    }
    if (m_current != clipId) return;
    m_current.clear();
    ++m_playSerial;
    playNext();
}

void VoiceSoundPlayer::reset()
{
    if (!m_current.isEmpty() && m_client) m_client->stop(m_current, this, {});
    m_current.clear();
    ++m_playSerial;
    m_earlyFinished.clear();
    m_waiting.clear();
}

void VoiceSoundPlayer::stop()
{
    m_stopping = true;
    reset();
    if (m_client) {
        m_client->finished = {};
        m_client->failed = {};
    }
}
}
