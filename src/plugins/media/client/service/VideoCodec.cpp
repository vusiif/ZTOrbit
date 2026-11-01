#include "VideoCodec.h"
#include <cstring>
#include <algorithm>
extern "C" {
#include <libavutil/opt.h>
}
namespace vusiif::zto::media::engine {
namespace {
QString avError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof buffer);
    return QString::fromUtf8(buffer);
}
} // namespace
QString VideoCodec::open(const TrackConfig &c, const ExtensionRegistry &registry) {
    close();
    config = c;
    const auto *codec = c.direction == "send" ? avcodec_find_encoder_by_name("libopenh264")
                                              : avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec)
        return "H.264 codec unavailable";
    context = avcodec_alloc_context3(codec);
    if (!context)
        return "Cannot allocate H.264 context";
    context->thread_count = 2;
    context->pkt_timebase = {1, 1000000};
    context->max_pixels = 1920 * 1080;
    if (c.direction == "send") {
        context->width = c.video.width;
        context->height = c.video.height;
        context->pix_fmt = AV_PIX_FMT_YUV420P;
        context->time_base = {1, c.codec.frameRate};
        context->framerate = {c.codec.frameRate, 1};
        context->bit_rate = c.codec.bitrate;
        context->gop_size = c.codec.keyFrameInterval;
        context->max_b_frames = 0;
        context->sample_aspect_ratio = {1, 1};
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        context->profile = FF_PROFILE_H264_MAIN;
        av_opt_set(context->priv_data, "allow_skip_frames", "1", 0);
        context->color_range = AVCOL_RANGE_MPEG;
        context->colorspace = AVCOL_SPC_BT709;
        context->color_primaries = AVCOL_PRI_BT709;
        context->color_trc = AVCOL_TRC_BT709;
    } else {
        // Frame threading retains the first IDR until another packet arrives.
        // Desktop capture can be static indefinitely; slice threading keeps
        // the negotiated no-B-frame stream decodable one packet at a time.
        context->thread_type = FF_THREAD_SLICE;
        if (!c.codec.initializationData.isEmpty()) {
        context->extradata_size = int(c.codec.initializationData.size());
        context->extradata =
            static_cast<uint8_t *>(av_mallocz(context->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!context->extradata)
            return "Cannot allocate codec initialization data";
        std::memcpy(context->extradata, c.codec.initializationData.constData(), context->extradata_size);
        }
    }
    int status = avcodec_open2(context, codec, nullptr);
    if (status < 0)
        return avError(status);
    return processors.create(c.processors, "video", registry);
}
void VideoCodec::close() {
    processors.clear();
    avcodec_free_context(&context);
    sws_freeContext(scale);
    scale = nullptr;
    firstPts = -1;
    nextPts = 0;
    sequence = 0;
    forceKeyFrame = true;
    waiting = true;
}
QList<EncodedPacket> VideoCodec::encode(const QImage &source, qint64 pts, QString &error) {
    QList<EncodedPacket> packets;
    if (!context || source.isNull())
        return packets;
    auto image = source.convertToFormat(QImage::Format_ARGB32);
    auto *frame = av_frame_alloc();
    if (!frame) {
        error = "Frame allocation failed";
        return {};
    }
    frame->width = context->width;
    frame->height = context->height;
    frame->format = context->pix_fmt;
    frame->time_base = context->time_base;
    frame->sample_aspect_ratio = {1, 1};
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        error = "Frame buffer allocation failed";
        return {};
    }
    scale =
        sws_getCachedContext(scale, image.width(), image.height(), AV_PIX_FMT_BGRA, frame->width,
                             frame->height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!scale) {
        av_frame_free(&frame);
        error = "Video conversion unavailable";
        return {};
    }
    const auto *coefficients = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(scale, coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16);
    const uint8_t *data[] = {image.constBits()};
    int strides[] = {int(image.bytesPerLine())};
    sws_scale(scale, data, strides, 0, image.height(), frame->data, frame->linesize);
    if (firstPts < 0)
        firstPts = pts;
    frame->pts = std::max(nextPts, av_rescale_q(pts - firstPts, {1, 1000000}, context->time_base));
    nextPts = frame->pts + 1;
    const auto inputFramePts = frame->pts;
    frame->pict_type = forceKeyFrame ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    forceKeyFrame = false;
    auto *processed = processors.process(frame, error);
    av_frame_free(&frame);
    if (!processed)
        return {};
    int result = avcodec_send_frame(context, processed);
    av_frame_free(&processed);
    if (result < 0) {
        error = avError(result);
        return {};
    }
    auto *packet = av_packet_alloc();
    if (!packet) {
        error = "Packet allocation failed";
        return {};
    }
    while ((result = avcodec_receive_packet(context, packet)) >= 0) {
        EncodedPacket p;
        p.codec = "h264";
        p.sequence = sequence++;
        // OpenH264 has no encoder delay: packets belong to this submitted
        // frame. Its integer frame grid must not move a forced refresh into
        // the future (e.g. a refresh after 100ms on a 1fps track). Preserve
        // actual input time, retaining any processor-relative PTS offset.
        p.pts = pts + av_rescale_q(packet->pts - inputFramePts, context->time_base, {1, 1000000});
        if (packet->dts != AV_NOPTS_VALUE)
            p.dts = pts + av_rescale_q(packet->dts - inputFramePts, context->time_base, {1, 1000000});
        p.duration = 1000000 / config.codec.frameRate;
        p.keyFrame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        if (context->extradata_size)
            p.initializationData =
                QByteArray(reinterpret_cast<const char *>(context->extradata), context->extradata_size);
        p.data = QByteArray(reinterpret_cast<const char *>(packet->data), packet->size);
        packets.append(std::move(p));
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF)
        error = avError(result);
    return packets;
}
QList<QPair<QImage, qint64>> VideoCodec::decode(const EncodedPacket &p, QString &error) {
    QList<QPair<QImage, qint64>> frames;
    if (!context)
        return frames;
    if (p.discontinuity) {
        avcodec_flush_buffers(context);
        waiting = true;
    }
    if (waiting && !p.keyFrame)
        return frames;
    waiting = false;
    auto *packet = av_packet_alloc();
    if (!packet)
        return frames;
    if (av_new_packet(packet, int(p.data.size())) < 0) {
        av_packet_free(&packet);
        return frames;
    }
    std::memcpy(packet->data, p.data.constData(), p.data.size());
    packet->pts = av_rescale_q(p.pts, {p.timeBase.numerator, p.timeBase.denominator}, {1, 1000000});
    packet->dts = p.dts ? av_rescale_q(*p.dts, {p.timeBase.numerator, p.timeBase.denominator}, {1, 1000000})
                        : packet->pts;
    int result = avcodec_send_packet(context, packet);
    av_packet_free(&packet);
    if (result < 0) {
        error = avError(result);
        waiting = true;
        return {};
    }
    auto *decoded = av_frame_alloc();
    if (!decoded)
        return {};
    while ((result = avcodec_receive_frame(context, decoded)) >= 0) {
        if (decoded->width < 1 || decoded->height < 1 || decoded->width > 1920 || decoded->height > 1080) {
            error = "Decoded video exceeds frame limits";
            break;
        }
        decoded->time_base={1,1000000};
        auto *processed = processors.process(decoded, error);
        av_frame_unref(decoded);
        if (!processed)
            continue;
        QImage image(processed->width, processed->height, QImage::Format_ARGB32);
        scale = sws_getCachedContext(scale, processed->width, processed->height,
                                     AVPixelFormat(processed->format), image.width(), image.height(),
                                     AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!scale) {
            av_frame_free(&processed);
            error = "Preview conversion failed";
            break;
        }
        const auto *coefficients =
            sws_getCoefficients(processed->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT);
        sws_setColorspaceDetails(scale, coefficients, processed->color_range == AVCOL_RANGE_JPEG,
                                 coefficients, 1, 0, 1 << 16, 1 << 16);
        uint8_t *data[] = {image.bits()};
        int strides[] = {int(image.bytesPerLine())};
        sws_scale(scale, processed->data, processed->linesize, 0, processed->height, data, strides);
        frames.append({image, processed->pts == AV_NOPTS_VALUE ? p.pts : processed->pts});
        av_frame_free(&processed);
    }
    av_frame_free(&decoded);
    if (result < 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF)
        error = avError(result);
    return frames;
}
} // namespace vusiif::zto::media::engine
