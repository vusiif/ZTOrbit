#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QtGlobal>
#include <memory>

#include <ztmedia/AudioClipClient.h>

namespace vusiif::ztgui::voice_chat {
// Serializes the two short room cues. Bursts merge adjacent waiting cues of the
// same kind, keep at most eight items (including the playing one) and discard
// waiting items older than two seconds on a monotonic clock.
class VoiceSoundPlayer final : public QObject {
    Q_OBJECT
public:
    enum class Cue { Join, Leave };
    VoiceSoundPlayer(std::shared_ptr<media::AudioClipClient> client,
                     QByteArray joinWav, QByteArray leaveWav, QObject *parent = nullptr);
    ~VoiceSoundPlayer() override;
    void setOutputDevice(const QByteArray &device);
    void setVolume(int percent);
    void setDeafened(bool deafened);
    void enqueue(Cue cue);
    void reset();
    void stop();

private:
    struct Item {
        Cue cue;
        qint64 queuedAt;
    };
    std::shared_ptr<media::AudioClipClient> m_client;
    QByteArray m_join, m_leave;
    QQueue<Item> m_waiting;
    QString m_current;
    Cue m_currentCue = Cue::Join;
    QSet<QString> m_earlyFinished;
    QByteArray m_device;
    int m_volume = 80;
    int m_playSerial = 0;
    bool m_deafened = false;
    bool m_stopping = false;
    QElapsedTimer m_clock;
    void prune();
    void playNext();
    void finish(const QString &clipId);
    void fail(const QString &clipId);
};
}
