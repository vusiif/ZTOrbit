#pragma once
#include <QAudioFormat>
#include <QByteArray>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <opus.h>
#include <ztmedia/MediaTypes.h>
extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
}
namespace vusiif::ztgui::media {
inline float clampSample(float value) { return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f; }
inline void applyGain(QVector<float> &samples, float gain) {
    gain = std::isfinite(gain) ? std::clamp(gain, 0.0f, 2.0f) : 0.0f;
    for (auto &sample : samples)
        sample = clampSample(sample * gain);
}
inline void mixStream(QVector<float> &mixed, const QVector<float> &frame, float gain) {
    gain = std::isfinite(gain) ? std::clamp(gain, 0.0f, 2.0f) : 0.0f;
    for (qsizetype i = 0; i < std::min(mixed.size(), frame.size()); ++i)
        mixed[i] += frame[i] * gain;
}
class Resampler {
    SwrContext *context = nullptr;
    int from = 0, to = 0;

  public:
    ~Resampler() { clear(); }
    void clear() {
        swr_free(&context);
        from = to = 0;
    }
    QVector<float> process(const QVector<float> &input, int sourceRate, int targetRate) {
        if (sourceRate <= 0 || targetRate <= 0)
            return {};
        if (!context || from != sourceRate || to != targetRate) {
            clear();
            AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
            if (swr_alloc_set_opts2(&context, &mono, AV_SAMPLE_FMT_FLT, targetRate, &mono, AV_SAMPLE_FMT_FLT,
                                    sourceRate, 0, nullptr) < 0 ||
                swr_init(context) < 0) {
                clear();
                return {};
            }
            from = sourceRate;
            to = targetRate;
        }
        QVector<float> output(swr_get_out_samples(context, int(input.size())));
        const uint8_t *in = reinterpret_cast<const uint8_t *>(input.constData());
        auto *out = reinterpret_cast<uint8_t *>(output.data());
        int count = swr_convert(context, &out, int(output.size()), &in, int(input.size()));
        if (count < 0)
            return {};
        output.resize(count);
        return output;
    }
};
inline QVector<float> decodePcm(QByteArray &pending, const QAudioFormat &format) {
    QVector<float> output;
    const int bytes = format.bytesPerSample(), channels = format.channelCount();
    const int frame = bytes * channels;
    if (frame <= 0)
        return output;
    const auto complete = pending.size() / frame;
    output.reserve(complete);
    for (qsizetype i = 0; i < complete; ++i) {
        float sum = 0;
        for (int channel = 0; channel < channels; ++channel) {
            const char *p = pending.constData() + i * frame + channel * bytes;
            float value = 0;
            switch (format.sampleFormat()) {
            case QAudioFormat::UInt8:
                value = (quint8(*p) - 128) / 128.0f;
                break;
            case QAudioFormat::Int16: {
                qint16 v;
                std::memcpy(&v, p, 2);
                value = v / 32768.0f;
                break;
            }
            case QAudioFormat::Int32: {
                qint32 v;
                std::memcpy(&v, p, 4);
                value = float(v / 2147483648.0);
                break;
            }
            case QAudioFormat::Float:
                std::memcpy(&value, p, 4);
                break;
            default:
                break;
            }
            sum += clampSample(value);
        }
        output.append(sum / channels);
    }
    pending.remove(0, complete * frame);
    return output;
}
inline QByteArray encodePcm(const QVector<float> &mono, const QAudioFormat &format, float volume) {
    QByteArray result;
    result.reserve(mono.size() * format.bytesPerFrame());
    for (float sample : mono) {
        const auto value = clampSample(sample * volume);
        for (int channel = 0; channel < format.channelCount(); ++channel) {
            switch (format.sampleFormat()) {
            case QAudioFormat::UInt8: {
                const quint8 v = quint8(std::clamp(int(value * 127 + 128), 0, 255));
                result.append(char(v));
                break;
            }
            case QAudioFormat::Int16: {
                const qint16 v = qint16(value * 32767);
                result.append(reinterpret_cast<const char *>(&v), 2);
                break;
            }
            case QAudioFormat::Int32: {
                const qint32 v = qint32(double(value) * 2147483647);
                result.append(reinterpret_cast<const char *>(&v), 4);
                break;
            }
            case QAudioFormat::Float:
                result.append(reinterpret_cast<const char *>(&value), 4);
                break;
            default:
                break;
            }
        }
    }
    return result;
}
class JitterStream {
    std::map<quint32, QByteArray> queue;
    OpusDecoder *decoder = nullptr;
    bool waiting = true, active = false;
    quint32 next = 0;
    int idleFrames = 0, waitFrames = 0;
    int startupFrames = 3;

  public:
    explicit JitterStream(int startup = 3) : startupFrames(startup) {
        int error;
        decoder = opus_decoder_create(48000, 1, &error);
    }
    ~JitterStream() {
        if (decoder)
            opus_decoder_destroy(decoder);
    }
    JitterStream(const JitterStream &) = delete;
    void push(quint32 sequence, const QByteArray &data) {
        if (data.isEmpty())
            return;
        if (!active) {
            next = sequence;
            active = true;
            waiting = true;
            waitFrames = 0;
        }
        const auto distance = qint32(sequence - next);
        if (distance < 0) {
            if (waiting && distance >= -3)
                next = sequence;
            else
                return;
        }
        if (distance > 25) {
            queue.clear();
            next = sequence;
            waiting = true;
            waitFrames = 0;
            if (decoder)
                opus_decoder_ctl(decoder, OPUS_RESET_STATE);
        }
        if (queue.size() < 25)
            queue.emplace(sequence, data);
    }
    QVector<float> pull() {
        QVector<float> output(FrameSamples, 0);
        if (!decoder || !active)
            return output;
        if (waiting && ++waitFrames <= startupFrames)
            return output;
        waiting = false;
        const auto it = queue.find(next++);
        int count = 0;
        if (it != queue.end()) {
            const auto data = it->second;
            // A single 20 ms packet is required; reject longer frames before decoding.
            if (opus_packet_get_nb_samples(reinterpret_cast<const unsigned char *>(data.constData()),
                                           int(data.size()), 48000) == FrameSamples)
                count = opus_decode_float(decoder, reinterpret_cast<const unsigned char *>(data.constData()),
                                          int(data.size()), output.data(), FrameSamples, 0);
            queue.erase(it);
            idleFrames = 0;
        } else {
            if (++idleFrames <= 5)
                count = opus_decode_float(decoder, nullptr, 0, output.data(), FrameSamples, 0);
            if (idleFrames > 25) {
                active = false;
                queue.clear();
            }
        }
        if (count < 0)
            output.fill(0);
        return output;
    }
};
} // namespace vusiif::ztgui::media
