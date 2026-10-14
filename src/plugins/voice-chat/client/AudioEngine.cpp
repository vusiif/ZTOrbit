#include "AudioEngine.h"
#include <QMediaDevices>

namespace vusiif::ztgui::voice_chat {
AudioEngine::AudioEngine(QObject *parent) : QObject(parent), playTimer(new QTimer(this))
{
    playTimer->setTimerType(Qt::PreciseTimer);
    connect(playTimer, &QTimer::timeout, this, &AudioEngine::play);
}
AudioEngine::~AudioEngine() { stop(); }
static QAudioDevice selectDevice(const QList<QAudioDevice> &devices, const QByteArray &id, const QAudioDevice &fallback) {
    for (const auto &device : devices) if (device.id() == id) return device;
    return fallback;
}
static QAudioFormat selectFormat(const QAudioDevice &device) {
    QAudioFormat format; format.setSampleRate(48000); format.setChannelCount(1); format.setSampleFormat(QAudioFormat::Float);
    return device.isFormatSupported(format) ? format : device.preferredFormat();
}
void AudioEngine::start(const QByteArray &inputId, const QByteArray &outputId, bool recordAllowed)
{
    stop();
    const auto input = selectDevice(QMediaDevices::audioInputs(), inputId, QMediaDevices::defaultAudioInput());
    const auto output = selectDevice(QMediaDevices::audioOutputs(), outputId, QMediaDevices::defaultAudioOutput());
    int error = OPUS_OK;
    encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error);
    if (!encoder || error != OPUS_OK) { emit notice(QStringLiteral("Opus 编码器初始化失败")); stop(); return; }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
    if (recordAllowed && !input.isNull()) {
        inputFormat = selectFormat(input);
        source = new QAudioSource(input, inputFormat, this);
        connect(source, &QAudioSource::stateChanged, this, [this](QtAudio::State state) {
            if (source && state == QtAudio::StoppedState && source->error() != QtAudio::NoError)
                emit notice(QStringLiteral("麦克风采集失败，请检查设备和系统权限"));
        });
        capture = source->start();
        if (capture) connect(capture, &QIODevice::readyRead, this, &AudioEngine::captured);
        else emit notice(QStringLiteral("麦克风启动失败，当前只接收语音"));
    } else emit notice(QStringLiteral("无可用麦克风或未授予权限，当前只接收语音"));
    if (!output.isNull()) {
        outputFormat = selectFormat(output);
        sink = new QAudioSink(output, outputFormat, this);
        sink->setVolume(deafened ? 0.0f : 1.0f);
        sink->setBufferSize(outputFormat.bytesForDuration(100000));
        connect(sink, &QAudioSink::stateChanged, this, [this](QtAudio::State state) {
            if (sink && state == QtAudio::StoppedState && sink->error() != QtAudio::NoError)
                emit notice(QStringLiteral("音频播放失败，请检查输出设备"));
        });
        playback = sink->start();
        if (!playback) emit notice(QStringLiteral("输出设备启动失败"));
    } else emit notice(QStringLiteral("无可用输出设备"));
    playTimer->start(20);
}
void AudioEngine::stop()
{
    playTimer->stop();
    if (source) { disconnect(source, nullptr, this, nullptr); if (capture) disconnect(capture, nullptr, this, nullptr); source->stop(); delete source; source = nullptr; }
    if (sink) { disconnect(sink, nullptr, this, nullptr); sink->stop(); delete sink; sink = nullptr; }
    capture = playback = nullptr;
    if (encoder) { opus_encoder_destroy(encoder); encoder = nullptr; }
    streams.clear(); inputPending.clear(); outputPending.clear(); capturePending.clear();
    inputResampler.clear(); outputResampler.clear();
}
void AudioEngine::setMuted(bool value) { muted = value; inputPending.clear(); capturePending.clear(); inputResampler.clear(); }
void AudioEngine::captured()
{
    if (!capture) return;
    const auto bytes = capture->readAll();
    if (muted || !encoder) return;
    inputPending += bytes;
    auto mono = decodePcm(inputPending, inputFormat);
    capturePending += inputResampler.process(mono, inputFormat.sampleRate(), 48000);
    if (capturePending.size() > FrameSamples * 5) capturePending.remove(0, capturePending.size() - FrameSamples * 5);
    while (capturePending.size() >= FrameSamples) {
        QByteArray encodedFrame(MaxDatagramBytes - HeaderBytes, '\0');
        auto frame=capturePending.mid(0,FrameSamples); applyGain(frame,inputVolume);
        const int size = opus_encode_float(encoder, frame.constData(), FrameSamples,
            reinterpret_cast<unsigned char *>(encodedFrame.data()), int(encodedFrame.size()));
        capturePending.remove(0, FrameSamples);
        if (size > 0) { encodedFrame.resize(size); emit encoded(encodedFrame); }
    }
}
void AudioEngine::receive(quint32 sender, quint32 sequence, const QByteArray &data)
{
    auto &stream = streams[sender];
    if (!stream) stream = std::make_unique<JitterStream>();
    stream->push(sequence, data);
}
void AudioEngine::retain(const std::set<quint32> &members)
{
    for(auto i=memberVolumes.begin();i!=memberVolumes.end();)
        if(!members.count(i->first)) i=memberVolumes.erase(i); else ++i;
    for (auto i = streams.begin(); i != streams.end(); )
        if (!members.count(i->first)) i = streams.erase(i); else ++i;
}
void AudioEngine::play()
{
    QVector<float> mix(FrameSamples, 0);
    for (auto &entry : streams) {
        const auto frame = entry.second->pull();
        const auto gain=memberVolumes.count(entry.first)?memberVolumes[entry.first]:1.0f;
        if(!deafened) mixStream(mix,frame,gain);
    }
    if (!playback || !sink) return;
    // Apply total output gain before the final PCM limiter.
    const auto converted = outputResampler.process(mix, 48000, outputFormat.sampleRate());
    outputPending += encodePcm(converted, outputFormat, volume);
    const auto maximum = outputFormat.bytesForDuration(100000);
    if (outputPending.size() > maximum) outputPending.remove(0, outputPending.size() - maximum);
    const auto bytes = std::min(qint64(outputPending.size()), sink->bytesFree());
    if (bytes > 0) {
        const auto written = playback->write(outputPending.constData(), bytes);
        if (written > 0) outputPending.remove(0, written);
    }
}
}
