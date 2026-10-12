#pragma once
#include "AudioMath.h"
#include <QObject>
#include <QAudioDevice>
#include <QAudioSource>
#include <QAudioSink>
#include <QTimer>
#include <memory>
#include <map>
#include <set>
namespace vusiif::ztgui::voice_chat {
class AudioEngine final : public QObject {
    Q_OBJECT
public:
    explicit AudioEngine(QObject *parent = nullptr);
    ~AudioEngine() override;
    void start(const QByteArray &inputId, const QByteArray &outputId, bool recordAllowed);
    void stop();
    void receive(quint32 sender, quint32 sequence, const QByteArray &data);
    void retain(const std::set<quint32> &members);
    void setMuted(bool value);
    void setDeafened(bool value) {
        deafened = value; outputPending.clear();
        if (sink) sink->setVolume(value ? 0.0f : 1.0f);
    }
    void setVolume(float value) { volume = std::clamp(value, 0.0f, 1.0f); }
signals:
    void encoded(const QByteArray &data);
    void notice(const QString &message);
private:
    QAudioSource *source = nullptr;
    QAudioSink *sink = nullptr;
    QIODevice *capture = nullptr, *playback = nullptr;
    QAudioFormat inputFormat, outputFormat;
    OpusEncoder *encoder = nullptr;
    QTimer *playTimer;
    bool muted = false, deafened = false;
    float volume = 0.8f;
    QByteArray inputPending, outputPending;
    QVector<float> capturePending;
    Resampler inputResampler, outputResampler;
    std::map<quint32, std::unique_ptr<JitterStream>> streams;
    void captured();
    void play();
};
}
