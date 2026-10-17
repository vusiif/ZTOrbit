#pragma once
#include <QAudioFormat>
#include <QByteArray>
extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
}
namespace vusiif::ztgui::media::engine {
class PcmConverter {
  public:
    ~PcmConverter() { swr_free(&context); }
    void clear() { swr_free(&context); }
    QByteArray process(const QByteArray &bytes, const QAudioFormat &input, const QAudioFormat &output) {
        if (input.bytesPerFrame() <= 0 || output.bytesPerFrame() <= 0)
            return {};
        if (!context || input != from || output != to) {
            clear();
            from = input;
            to = output;
            AVChannelLayout in{}, out{};
            av_channel_layout_default(&in, input.channelCount());
            av_channel_layout_default(&out, output.channelCount());
            int status = swr_alloc_set_opts2(&context, &out, format(output), output.sampleRate(), &in,
                                             format(input), input.sampleRate(), 0, nullptr);
            av_channel_layout_uninit(&in);
            av_channel_layout_uninit(&out);
            if (status < 0 || swr_init(context) < 0) {
                clear();
                return {};
            }
        }
        const int count = int(bytes.size() / input.bytesPerFrame()),
                  capacity = swr_get_out_samples(context, count);
        QByteArray result(qsizetype(capacity) * output.bytesPerFrame(), Qt::Uninitialized);
        const auto *in = reinterpret_cast<const uint8_t *>(bytes.constData());
        auto *out = reinterpret_cast<uint8_t *>(result.data());
        int converted = swr_convert(context, &out, capacity, &in, count);
        if (converted < 0)
            return {};
        result.resize(qsizetype(converted) * output.bytesPerFrame());
        return result;
    }
    static QAudioFormat mono() {
        QAudioFormat f;
        f.setSampleRate(48000);
        f.setChannelCount(1);
        f.setSampleFormat(QAudioFormat::Float);
        return f;
    }

  private:
    SwrContext *context = nullptr;
    QAudioFormat from, to;
    static AVSampleFormat format(const QAudioFormat &f) {
        switch (f.sampleFormat()) {
        case QAudioFormat::UInt8:
            return AV_SAMPLE_FMT_U8;
        case QAudioFormat::Int16:
            return AV_SAMPLE_FMT_S16;
        case QAudioFormat::Int32:
            return AV_SAMPLE_FMT_S32;
        case QAudioFormat::Float:
            return AV_SAMPLE_FMT_FLT;
        default:
            return AV_SAMPLE_FMT_NONE;
        }
    }
};
} // namespace vusiif::ztgui::media::engine
