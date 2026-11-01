#include "SessionEngine.h"
#include "VideoCanvas.h"
#include "VideoFramePacer.h"
#include "VideoCodec.h"
#include "TileCodec.h"
#include "AudioMath.h"
#include "MediaClock.h"
#include "ClipAudio.h"
#include "ClipPlayer.h"
#include <ztmedia/SharedFrames.h>
#include <QElapsedTimer>
#include <QPointer>
#include <QRegularExpression>
#include <QUuid>
#include <QPainter>
#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>

namespace vusiif::zto::media::engine {
namespace {
qint64 clockUs(){return monotonicTimeUs();}
QString validate(TrackConfig &c) {
    if (c.direction != "send" && c.direction != "receive")
        return "Unsupported track direction";
    if (!std::isfinite(c.gain) || c.gain < 0 || c.gain > 4)
        return "Gain must be within 0..4";
    if (c.kind == "audio") {
        if (c.codec.codec != "opus" || c.audio.sampleRate != 48000 || c.audio.channels != 1 ||
            c.audio.sampleFormat != "f32")
            return "Audio requires Opus, 48000 Hz mono f32";
        if (c.codec.bitrate < 6000 || c.codec.bitrate > 128000)
            return "Invalid Opus bitrate";
    } else if (c.kind == "video") {
        const bool tiles = c.codec.codec == "bgra-tiles";
        if ((c.codec.codec != "h264" && !tiles) || c.codec.preferHardware)
            return "This build provides software H.264 and BGRA tiles";
        if (tiles && !c.codec.initializationData.isEmpty()) return "Tile tracks require empty initialization data";
        if (tiles && !c.processors.isEmpty()) return "Tile tracks do not support processors";
        if (tiles && !c.syncGroup.isEmpty()) return "Tile tracks use immediate presentation without A/V sync groups";
        if (c.video.width < 16 || c.video.height < 16 || c.video.width > 1920 || c.video.height > 1080 ||
            c.video.width % 2 || c.video.height % 2 || c.video.pixelFormat != "bgra" ||
            c.video.colorSpace != "bt709" || c.video.colorRange != (tiles ? "full" : "limited"))
            return "Unsupported video format";
        if (c.codec.frameRate < 1 || c.codec.frameRate > 60 || c.codec.bitrate < 64000 ||
            c.codec.bitrate > 20000000 || c.codec.keyFrameInterval < 1 || c.codec.keyFrameInterval > 300)
            return "Invalid video encoding parameters";
    } else
        return "Unsupported media kind";
    return {};
}
struct VideoPermit {
    std::shared_ptr<std::atomic<int>> count;
    explicit VideoPermit(std::shared_ptr<std::atomic<int>> value) : count(std::move(value)) {}
    ~VideoPermit() { count->fetch_sub(1); }
};
struct VideoMailbox {
    std::mutex mutex;
    QVideoFrame latest;
    bool queued = false;
    quint64 replaced = 0;
    std::atomic<quint64> offered{0}, delivered{0}, replacements{0}, queueDrops{0}, cancellations{0};
};
struct Track {
    QString id;
    TrackConfig config;
    quint64 configuration = 1, sequence = 0, captured = 0, received = 0, dropped = 0, frames = 0;
    quint64 processingUs = 0, peakProcessingUs = 0;
    quint64 decodedOutput = 0, decodedQueueDrops = 0, decodedQueuePeak = 0;
    quint64 playbackReanchors = 0;
    quint64 decodeUs = 0, peakDecodeUs = 0, tickCount = 0, tickDrained = 0;
    quint64 previewPublished = 0, previewPublishFailures = 0, previewOverwritten = 0;
    quint64 previewDataFailures = 0, previewUs = 0, peakPreviewUs = 0;
    quint64 keyFrameRefreshes = 0;
    quint64 videoBeforeStart = 0;
    quint64 actorVideoFrames = 0, invalidVideoFrames = 0;
    quint64 startupFramesUsed = 0;
    std::shared_ptr<VideoMailbox> mailbox;
    QVideoFrame startupFrame;
    QImage lastVideoCanvas;
    qint64 nextKeyFrameRefreshUs = 0;
    qint64 firstDecodedPts = 0, lastDecodedPts = 0, lastPacketPts = 0;
    qint64 scheduleAheadUs = 0, peakScheduleAheadUs = 0, lastTickUs = 0, peakTickGapUs = 0;
    bool running = false, muted = false, discontinuous = false;
    qint64 nextTick = 0, audioPts = 0, anchor = -1, remoteAnchor = 0;
    VideoFramePacer framePacer;
    std::unique_ptr<VideoCodec> video;
    std::unique_ptr<TileEncoder> tileEncoder;
    std::unique_ptr<TileDecoder> tileDecoder;
    qint64 lastRefreshRequestUs = 0;
    std::shared_ptr<VideoPermit> videoPermit;
    OpusEncoder *encoder = nullptr;
    std::unique_ptr<JitterStream> jitter;
    PcmConverter converter;
    QByteArray input;
    ProcessorChain processors;
    std::unique_ptr<detail::SharedFrames> preview;
    QQueue<QPair<QImage, qint64>> decoded;
    ~Track() {
        if (encoder)
            opus_encoder_destroy(encoder);
    }
};
} // namespace
class SessionActor final : public QObject {
  public:
    using Reply = SessionEngine::Reply;
    QTimer *timer;
    ExtensionRegistry &extensions;
    std::map<QString, std::unique_ptr<Track>> tracks;
    PlaybackTimeline timeline;
    quint64 generation;
    std::shared_ptr<std::atomic<int>> videoCount;
    std::function<bool(QJsonObject, QByteArray)> data;
    std::function<void(QString, QString)> failed;
    std::function<void(QString, QString, QVector<float>)> playback;
    explicit SessionActor(ExtensionRegistry &e, quint64 g, std::shared_ptr<std::atomic<int>> budget)
        : timer(new QTimer(this)), extensions(e), generation(g), videoCount(std::move(budget)) {
        timer->setTimerType(Qt::PreciseTimer);
        timer->setInterval(5);
        connect(timer, &QTimer::timeout, this, [this] { tick(); });
    }
    QString initialize(Track &t, std::shared_ptr<VideoPermit> permit = {}) {
        auto error = validate(t.config);
        if (!error.isEmpty())
            return error;
        if (t.config.kind == "video") {
            if (!permit) {
                if (videoCount->fetch_add(1) >= 16) {
                    videoCount->fetch_sub(1);
                    return "Worker video pipeline limit reached";
                }
                permit = std::make_shared<VideoPermit>(videoCount);
            }
            t.videoPermit = std::move(permit);
            if (t.config.codec.codec == "bgra-tiles") {
                if (t.config.direction == "send") t.tileEncoder = std::make_unique<TileEncoder>(t.config);
                else t.tileDecoder = std::make_unique<TileDecoder>(t.config);
            } else {
                t.video = std::make_unique<VideoCodec>();
                error = t.video->open(t.config, extensions);
            }
            if (!error.isEmpty())
                return error;
        } else {
            if (t.config.direction == "send") {
                int code = 0;
                t.encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &code);
                if (!t.encoder || code != OPUS_OK)
                    return "Cannot create Opus encoder";
                opus_encoder_ctl(t.encoder, OPUS_SET_BITRATE(t.config.codec.bitrate));
                opus_encoder_ctl(t.encoder, OPUS_SET_COMPLEXITY(5));
            } else
                t.jitter = std::make_unique<JitterStream>(0);
            error = t.processors.create(t.config.processors, "audio", extensions);
            if (!error.isEmpty())
                return error;
        }
        if (t.config.kind == "video" && t.config.preview) {
            t.preview = std::make_unique<detail::SharedFrames>();
            if (!t.preview->create(generation))
                return "Cannot allocate shared preview frames";
        }
        return {};
    }
    QJsonObject descriptor(const Track &t) {
        return {
            {"id", t.id}, {"configuration", QString::number(t.configuration)}, {"config", toJson(t.config)}};
    }
    void command(const QString &method, const QJsonObject &d, Reply reply) {
        if (method == "statistics") {
            QJsonArray values;
            for (const auto &p : tracks) {
                const auto &t = *p.second;
                values.append(QJsonObject{{"id", t.id},
                                          {"running", t.running},
                                          {"captured", QString::number(t.captured)},
                                          {"received", QString::number(t.received)},
                                          {"frames", QString::number(t.frames)},
                                          {"processingUs", QString::number(t.processingUs)},
                                          {"peakProcessingUs", QString::number(t.peakProcessingUs)},
                                          {"dropped", QString::number(t.dropped)},
                                          {"direction", t.config.direction},
                                          {"decodedOutput", QString::number(t.decodedOutput)},
                                          {"decodedQueueDrops", QString::number(t.decodedQueueDrops)},
                                          {"playbackReanchors", QString::number(t.playbackReanchors)},
                                          {"decodedQueueSize", t.decoded.size()},
                                          {"decodedQueuePeak", QString::number(t.decodedQueuePeak)},
                                          {"decodeUs", QString::number(t.decodeUs)},
                                          {"peakDecodeUs", QString::number(t.peakDecodeUs)},
                                          {"tickCount", QString::number(t.tickCount)},
                                          {"tickDrained", QString::number(t.tickDrained)},
                                          {"peakTickGapUs", QString::number(t.peakTickGapUs)},
                                          {"firstDecodedPts", QString::number(t.firstDecodedPts)},
                                          {"lastDecodedPts", QString::number(t.lastDecodedPts)},
                                          {"lastPacketPts", QString::number(t.lastPacketPts)},
                                          {"scheduleAheadUs", QString::number(t.scheduleAheadUs)},
                                          {"peakScheduleAheadUs", QString::number(t.peakScheduleAheadUs)},
                                          {"frontAheadUs", QString::number(t.decoded.isEmpty() ? 0 : t.decoded.front().second - clockUs())},
                                          {"previewPublished", QString::number(t.previewPublished)},
                                          {"previewPublishFailures", QString::number(t.previewPublishFailures)},
                                          {"previewOverwritten", QString::number(t.previewOverwritten)},
                                          {"previewDataFailures", QString::number(t.previewDataFailures)},
                                          {"previewUs", QString::number(t.previewUs)},
                                          {"peakPreviewUs", QString::number(t.peakPreviewUs)},
                                          {"keyFrameRefreshes", QString::number(t.keyFrameRefreshes)}});
                auto startSnapshot=values.last().toObject();
                startSnapshot["videoBeforeStart"]=QString::number(t.videoBeforeStart);
                startSnapshot["actorVideoFrames"]=QString::number(t.actorVideoFrames);
                startSnapshot["invalidVideoFrames"]=QString::number(t.invalidVideoFrames);
                startSnapshot["startupFramesUsed"]=QString::number(t.startupFramesUsed);
                if (t.mailbox) {
                    startSnapshot["mailboxOffered"]=QString::number(t.mailbox->offered.load());
                    startSnapshot["mailboxDelivered"]=QString::number(t.mailbox->delivered.load());
                    startSnapshot["mailboxReplaced"]=QString::number(t.mailbox->replacements.load());
                    startSnapshot["mailboxQueueDrops"]=QString::number(t.mailbox->queueDrops.load());
                    startSnapshot["mailboxCancelled"]=QString::number(t.mailbox->cancellations.load());
                }
                values.replace(values.size()-1,startSnapshot);
                if (t.tileEncoder || t.tileDecoder) {
                    const auto extra = t.tileEncoder ? t.tileEncoder->statistics() : t.tileDecoder->statistics();
                    auto snapshot = values.last().toObject();
                    for (auto field = extra.begin(); field != extra.end(); ++field) snapshot.insert(field.key(),field.value());
                    values.replace(values.size()-1,snapshot);
                }
            }
            reply({{"tracks", values}}, {}, {});
            return;
        }
        if (method == "track.create") {
            if (tracks.size() >= 128) {
                reply({}, {}, "Track limit reached");
                return;
            }
            auto t = std::make_unique<Track>();
            t->id = QString::number(generation) + "-" + QUuid::createUuid().toString(QUuid::Id128);
            t->config = trackConfig(d["config"].toObject());
            auto error = initialize(*t);
            if (!error.isEmpty()) {
                reply({}, {}, error);
                return;
            }
            auto result = descriptor(*t);
            tracks.emplace(t->id, std::move(t));
            reply(result, {}, {});
            return;
        }
        const auto id = d["id"].toString();
        auto it = tracks.find(id);
        if (it == tracks.end()) {
            reply({}, {}, "Unknown or stale track");
            return;
        }
        auto &t = *it->second;
        if (method == "track.configure") {
            if (t.running) {
                reply({}, {}, "Stop the track before reconfiguring");
                return;
            }
            auto replacement = std::make_unique<Track>();
            replacement->id = t.id;
            replacement->config = trackConfig(d["config"].toObject());
            replacement->configuration = t.configuration + 1;
            auto error = initialize(*replacement, t.videoPermit);
            if (!error.isEmpty()) {
                reply({}, {}, error);
                return;
            }
            it->second = std::move(replacement);
            reply(descriptor(*it->second), {}, {});
            return;
        }
        if (method == "track.start") {
            if (!t.running) t.framePacer.reset();
            t.running = true;
            // Device attach can already have delivered the sole static frame.
            // Preserve it, including a capture timestamp earlier than start.
            t.nextTick = t.config.kind == "video" && t.config.source != "synthetic" ? 0 : clockUs();
            if (t.startupFrame.isValid()) {
                const auto first = std::exchange(t.startupFrame, {});
                ++t.startupFramesUsed;
                image(id, first.toImage(), first.startTime());
            }
            timer->start();
        } else if (method == "track.stop") {
            t.running = false;
            t.framePacer.reset();
            t.startupFrame = {};
            t.input.clear();
            t.decoded.clear();
            t.lastVideoCanvas = {};
            t.nextKeyFrameRefreshUs = 0;
            if (t.tileEncoder) t.tileEncoder->stop();
            if (t.tileDecoder) t.tileDecoder = std::make_unique<TileDecoder>(t.config);
            t.converter.clear();
            t.anchor = -1;
            t.audioPts = 0;
            if (t.jitter)
                t.jitter = std::make_unique<JitterStream>(0);
            if (t.video) {
                auto error = t.video->open(t.config, extensions);
                if (!error.isEmpty()) {
                    reply({}, {}, error);
                    return;
                }
            }
        } else if (method == "track.remove") {
            auto group = t.config.syncGroup.isEmpty() ? t.id : t.config.syncGroup;
            tracks.erase(it);
            bool used = false;
            for (const auto &entry : tracks)
                used |= (entry.second->config.syncGroup.isEmpty() ? entry.second->id
                                                                  : entry.second->config.syncGroup) == group;
            if (!used)
                timeline.reset(group);
        } else if (method == "track.keyframe") {
            if ((!t.video && !t.tileEncoder) || t.config.direction != "send") {
                reply({}, {}, "Key frames require a video send track");
                return;
            }
            if (t.video) t.video->keyFrame();
            const auto now = clockUs();
            // A static desktop may never deliver another capture frame. Keep
            // one bounded canvas and refresh it on demand, without claiming a
            // new capture or feeding a periodic duplicate-frame loop.
            if (t.running && !t.lastVideoCanvas.isNull() && now >= t.nextKeyFrameRefreshUs
                && t.framePacer.admit(now, t.config.codec.frameRate)) {
                t.nextKeyFrameRefreshUs = now + 1000000 / t.config.codec.frameRate;
                ++t.keyFrameRefreshes;
                // Cached refreshes share capture's budget; requests cannot
                // finance additional encodings between live captures.
                if (t.tileEncoder) t.tileEncoder->keyFrame();
                encodeCanvas(t, t.lastVideoCanvas, now);
                const auto elapsed = quint64(clockUs() - now);
                t.processingUs += elapsed;
                t.peakProcessingUs = std::max(t.peakProcessingUs, elapsed);
            }
        } else if (method == "track.gain") {
            double gain = d["gain"].toDouble(-1);
            if (!std::isfinite(gain) || gain < 0 || gain > 4) {
                reply({}, {}, "Invalid gain");
                return;
            }
            t.config.gain=float(gain);
            if(t.config.direction=="send"&&t.muted!=d["muted"].toBool()){t.input.clear();t.converter.clear();t.audioPts=0;t.discontinuous=true;}
            t.muted=d["muted"].toBool();
        } else {
            reply({}, {}, "Unknown session command");
            return;
        }
        bool active = false;
        for (const auto &p : tracks)
            active |= p.second->running;
        if (!active)
            timer->stop();
        reply({}, {}, {});
    }
    void emitPacket(Track &t, EncodedPacket p) {
        p.track = t.id;
        p.generation = generation;
        p.configuration = t.configuration;
        p.discontinuity = t.discontinuous;
        t.discontinuous = false;
        auto h = toJson(p);
        h["type"] = "packet";
        if (data && !data(h, p.data)) {
            ++t.dropped;
            t.discontinuous = true;
            if (t.video)
                t.video->keyFrame();
            if (t.tileEncoder) t.tileEncoder->keyFrame();
        }
    }
    void preview(Track &t, const QImage &source, qint64 pts, bool decoded) {
        if (!t.preview)
            return;
        auto image = source.convertToFormat(QImage::Format_ARGB32);
        auto size = image.sizeInBytes();
        const auto serial = t.frames++;
        const auto previous = t.preview->overwritten();
        const auto publishStarted = clockUs();
        int slot = t.preview->publish(image.constBits(), size, serial);
        const auto elapsed = quint64(clockUs() - publishStarted);
        t.previewUs += elapsed;
        t.peakPreviewUs = std::max(t.peakPreviewUs, elapsed);
        const auto overwritten = t.preview->overwritten() - previous;
        t.previewOverwritten += overwritten;
        t.dropped += overwritten;
        if (slot < 0) {
            ++t.previewPublishFailures;
            ++t.dropped;
            return;
        }
        QJsonObject h{{"type", "frame"},
                      {"track", t.id},
                      {"generation", QString::number(generation)},
                      {"configuration", QString::number(t.configuration)},
                      {"sequence", QString::number(serial)},
                      {"pts", QString::number(pts)},
                      {"duration", QString::number(1000000 / t.config.codec.frameRate)},
                      {"width", image.width()},
                      {"height", image.height()},
                      {"stride", image.bytesPerLine()},
                      {"size", double(size)},
                      {"mapping", t.preview->key()},
                      {"slot", slot},
                      {"decoded", decoded}};
        if (!data || !data(h, {})) {
            ++t.previewDataFailures;
            t.preview->discard(slot, serial);
        } else {
            ++t.previewPublished;
        }
    }
    void video(const QString &id, QVideoFrame frame) {
        auto it=tracks.find(id);
        if (it != tracks.end()) {
            ++it->second->actorVideoFrames;
            if (!frame.isValid()) ++it->second->invalidVideoFrames;
        }
        if (frame.isValid() && it != tracks.end() && !it->second->running) {
            ++it->second->videoBeforeStart;
            it->second->startupFrame = std::move(frame);
            return;
        }
        if (frame.isValid())
            image(id, frame.toImage(),frame.startTime());
    }
    void encodeCanvas(Track &t, const QImage &canvas, qint64 pts) {
        preview(t, canvas, pts, false);
        if (t.tileEncoder) {
            t.tileEncoder->capture(canvas,pts);
            for (auto packet : t.tileEncoder->drain(clockUs())) emitPacket(t,std::move(packet));
            return;
        }
        QString error;
        for (auto packet : t.video->encode(canvas, pts, error))
            emitPacket(t, std::move(packet));
        if (!error.isEmpty() && failed)
            failed(t.id, error);
    }
    void image(const QString &id, const QImage &image,qint64 capturedAt=-1) {
        auto it = tracks.find(id);
        if (it == tracks.end() || !it->second->running)
            return;
        auto &t = *it->second;
        auto now=clockUs();const auto pts=capturedAt>=0?capturedAt:now;
        if (!t.framePacer.admit(pts, t.config.codec.frameRate)) {
            ++t.dropped;
            return;
        }
        ++t.captured;
        // Encode and preview always consume the same fixed-size canvas so a
        // 4:3, 16:10, ultrawide or portrait source keeps its shape with black
        // bars instead of being stretched to the track format.
        auto canvas = composeVideoCanvas(image, t.config.video.width, t.config.video.height);
        t.lastVideoCanvas = canvas;
        encodeCanvas(t, canvas, pts);
        auto elapsed = quint64(clockUs() - now);
        t.processingUs += elapsed;
        t.peakProcessingUs = std::max(t.peakProcessingUs, elapsed);
    }
    void audio(const QString &id, const QByteArray &bytes, const QAudioFormat &format,qint64 capturedAt) {
        auto it = tracks.find(id);
        if (it == tracks.end() || !it->second->running)
            return;
        auto &t = *it->second;
        if (t.muted)
            return;
        t.input += t.converter.process(bytes, format, PcmConverter::mono());
        if (t.input.size() > FrameSamples * 4 * 5) {
            const auto removed=t.input.size()-FrameSamples*4*5;t.input.remove(0,removed);
            if(t.audioPts)t.audioPts+=(removed/4)*1000000/48000;t.discontinuous=true;++t.dropped;
        }
        if(!t.audioPts)t.audioPts=capturedAt-(t.input.size()/4)*1000000/48000;
        while (t.input.size() >= FrameSamples * 4) {
            AVFrame *frame = av_frame_alloc();
            if (!frame)
                return;
            frame->format = AV_SAMPLE_FMT_FLT;
            frame->sample_rate = 48000;
            frame->nb_samples = FrameSamples;
            const auto framePts=t.audioPts;t.audioPts+=20000;frame->pts=framePts;frame->time_base={1,1000000};
            av_channel_layout_default(&frame->ch_layout, 1);
            if (av_frame_get_buffer(frame, 0) < 0) {
                av_frame_free(&frame);
                return;
            }
            std::memcpy(frame->data[0], t.input.constData(), FrameSamples * 4);
            t.input.remove(0, FrameSamples * 4);
            QString error;
            auto *processed = t.processors.process(frame, error);
            av_frame_free(&frame);
            if (!processed) {
                if (!error.isEmpty() && failed)
                    failed(id, error);
                continue;
            }
            if (processed->format != AV_SAMPLE_FMT_FLT || processed->nb_samples != FrameSamples ||
                processed->ch_layout.nb_channels != 1) {
                av_frame_free(&processed);
                if (failed)
                    failed(id, "Processor changed the negotiated audio format");
                continue;
            }
            if (av_frame_make_writable(processed) < 0) {
                av_frame_free(&processed);
                return;
            }
            auto *samples = reinterpret_cast<float *>(processed->data[0]);
            for (int i = 0; i < FrameSamples; ++i)
                samples[i] = clampSample(samples[i] * t.config.gain);
            QByteArray encoded(MaxOpusBytes, Qt::Uninitialized);
            int count = opus_encode_float(t.encoder, samples, FrameSamples,
                                          reinterpret_cast<unsigned char *>(encoded.data()), MaxOpusBytes);
            av_frame_free(&processed);
            if (count > 0) {
                encoded.resize(count);
                EncodedPacket p;
                p.codec = "opus";
                p.sequence = t.sequence++;
                p.pts=framePts;
                p.data = std::move(encoded);
                ++t.captured;
                emitPacket(t, std::move(p));
            }
        }
    }
    void submit(const QJsonObject &h, const QByteArray &bytes) {
        auto p = encodedPacket(h, bytes);
        auto it = tracks.find(p.track);
        if (it == tracks.end() || p.generation != generation || p.configuration != it->second->configuration)
            return;
        auto &t = *it->second;
        if (!t.running || t.config.direction != "receive" || p.codec != t.config.codec.codec ||
            p.timeBase.numerator <= 0 || p.timeBase.denominator <= 0 || bytes.isEmpty())
            return;
        ++t.received;
        if (t.config.kind == "audio") {
            if (bytes.size() > MaxOpusBytes)
                return;
            auto group = t.config.syncGroup.isEmpty() ? t.id : t.config.syncGroup;
            if(t.anchor<0||p.discontinuity){t.anchor=timeline.schedule(group,p.pts,p.timeBase,clockUs());t.nextTick=t.anchor;if(p.discontinuity)t.jitter=std::make_unique<JitterStream>(0);}
            t.jitter->push(quint32(p.sequence), bytes);
        } else {
            QString error;
            const auto decodeStarted = clockUs();
            t.lastPacketPts = p.pts;
            auto decoded = t.tileDecoder ? t.tileDecoder->decode(p,clockUs(),error) : t.video->decode(p,error);
            const auto elapsed = quint64(clockUs() - decodeStarted);
            t.decodeUs += elapsed;
            t.peakDecodeUs = std::max(t.peakDecodeUs, elapsed);
            if (!error.isEmpty()) {
                if (t.tileDecoder) ++t.dropped;
                else if (failed) failed(t.id,error);
            }
            for (auto &f : decoded) {
                if (t.decodedOutput++ == 0) t.firstDecodedPts = f.second;
                t.lastDecodedPts = f.second;
                auto group = t.config.syncGroup.isEmpty() ? t.id : t.config.syncGroup;
                const auto now = clockUs();
                const auto remotePts = f.second;
                // Absolute desktop updates are presented as soon as complete;
                // capture timestamps remain in packets for diagnostics. They
                // do not use the synchronized A/V playout timeline.
                f.second=t.tileDecoder ? now : timeline.schedule(group,f.second,{1,1000000},now);
                if (t.decoded.size() >= 3) {
                    // A slow first packet can anchor otherwise live video far
                    // ahead of now. Evicting the oldest of three future frames
                    // then prevents any frame from ever reaching its deadline.
                    // Recover an independent video timeline at the latest input;
                    // shared A/V anchors and the three-frame bound stay intact.
                    const auto maximumLead = PlaybackTimeline::TargetDelayUs + 3000000 / t.config.codec.frameRate;
                    if (t.video && t.config.syncGroup.isEmpty() && t.decoded.front().second > now &&
                        f.second - now > maximumLead) {
                        t.decodedQueueDrops += t.decoded.size();
                        t.dropped += t.decoded.size();
                        t.decoded.clear();
                        timeline.reset(group);
                        f.second = timeline.schedule(group, remotePts, {1,1000000}, now);
                        ++t.playbackReanchors;
                    } else {
                        t.decoded.dequeue();
                        ++t.decodedQueueDrops;
                        ++t.dropped;
                    }
                }
                t.scheduleAheadUs = f.second - now;
                t.peakScheduleAheadUs = std::max(t.peakScheduleAheadUs, t.scheduleAheadUs);
                t.decoded.enqueue(std::move(f));
                t.decodedQueuePeak = std::max(t.decodedQueuePeak, quint64(t.decoded.size()));
            }
        }
    }
    void tick() {
        auto now = clockUs();
        for (auto &p : tracks) {
            auto &t = *p.second;
            if (!t.running)
                continue;
            ++t.tickCount;
            if (t.lastTickUs) t.peakTickGapUs = std::max(t.peakTickGapUs, now - t.lastTickUs);
            t.lastTickUs = now;
            if (t.tileEncoder)
                for (auto packet : t.tileEncoder->drain(now)) emitPacket(t,std::move(packet));
            if (t.tileDecoder && t.tileDecoder->refreshRequired(now) && now-t.lastRefreshRequestUs >= 1000000) {
                if (data && data({{"type","keyframe-required"},{"track",t.id},
                                  {"generation",QString::number(generation)},
                                  {"configuration",QString::number(t.configuration)}},{}))
                    t.lastRefreshRequestUs = now;
            }
            if (t.config.kind == "audio" && t.config.direction == "receive" && t.anchor >= 0 &&
                now >= t.nextTick) {
                t.nextTick = std::max(t.nextTick + 20000, now - 20000);
                auto frame = t.jitter->pull();
                for (auto &sample : frame)
                    sample = std::isfinite(sample) ? sample * (t.muted ? 0.f : t.config.gain) : 0.f;
                if (playback)
                    playback(t.id, t.config.output, std::move(frame));
            }
            if (t.config.kind == "video" && t.config.direction == "send" && t.config.source == "synthetic" &&
                now >= t.nextTick) {
                // Synthetic sources have an upstream timer too. Advance it
                // explicitly, instead of encoding at every 5ms actor tick.
                t.nextTick = std::max(t.nextTick + 1000000 / t.config.codec.frameRate, now);
                QImage image(t.config.video.width, t.config.video.height, QImage::Format_ARGB32);
                image.fill(QColor::fromHsv(int(t.captured * 3 % 360), 180, 180));
                QPainter painter(&image);
                painter.fillRect(0, 0, image.width() / 3, image.height(), Qt::red);
                painter.end();
                imageFunc(t.id, image);
            }
            while (!t.decoded.isEmpty() && t.decoded.front().second <= now) {
                auto f = t.decoded.dequeue();
                ++t.tickDrained;
                preview(t, f.first, f.second, true);
            }
        }
    }
    void imageFunc(const QString &id, const QImage &imageValue) { image(id, imageValue); }
};
SessionEngine::SessionEngine(protocol::service::Worker &w, DeviceManager &d, ExtensionRegistry &e,
                             QObject *parent)
    : QObject(parent), worker(w), devices(d), extensions(e) {
    QMetaObject::invokeMethod(&devices, [this] {
        devices.changed = [this] {
            QMetaObject::invokeMethod(this, [this] {
                for (const auto &s : sessions)
                    worker.event(s.first, {{"event", "sources"}});
            });
        };
    });
}
SessionEngine::~SessionEngine() { shutdown(); }
SessionEngine::Session *SessionEngine::session(const QString &lease) {
    auto found = sessions.find(lease);
    if (found != sessions.end())
        return found->second.get();
    if (sessions.size() >= 32)
        return nullptr;
    auto s = std::make_unique<Session>();
    s->thread = std::make_unique<QThread>();
    s->actor = new SessionActor(extensions, worker.generation().toULongLong(), videoCount);
    s->actor->moveToThread(s->thread.get());
    connect(s->thread.get(), &QThread::finished, s->actor, &QObject::deleteLater);
    s->actor->failed = [this, lease](QString id, QString message) {
        QMetaObject::invokeMethod(this, [this, lease, id, message] {
            worker.event(lease, {{"event", "error"}, {"code", "track"}, {"track", id}, {"message", message}});
        });
    };
    s->actor->playback = [this, lease](QString id, QString output, QVector<float> samples) {
        QMetaObject::invokeMethod(this, [this, lease, id, output, samples = std::move(samples)]() mutable {
            auto s = sessions.find(lease);
            if (s != sessions.end() && !s->second->closing && s->second->active.contains(id))
                QMetaObject::invokeMethod(&devices,
                                          [this, id, output, samples = std::move(samples)]() mutable {
                                              devices.play(id, output, std::move(samples));
                                          });
        });
    };
    auto queued = s->queued;
    s->actor->data = [this, lease, queued](QJsonObject h, QByteArray p) {
        if (!queued->reserve(p.size()))
            return false;
        if (queued->fetch_add(1) >= 32) {
            queued->fetch_sub(1);
            queued->release(p.size());
            return false;
        }
        QMetaObject::invokeMethod(this, [this, lease, queued, h = std::move(h), p = std::move(p)] {
            queued->fetch_sub(1);
            queued->release(p.size());
            if (!worker.sendData(lease, h, p)) {
                if (h["type"] == "frame") {
                    detail::SharedFrames m;
                    if (m.open(h["mapping"].toString(), worker.generation().toULongLong()))
                        m.discard(h["slot"].toInt(), h["sequence"].toString().toULongLong());
                } else {
                    auto it = sessions.find(lease);
                    if (it != sessions.end() && !it->second->closing) {
                        auto *actor = it->second->actor;
                        auto id = h["track"].toString();
                        QMetaObject::invokeMethod(actor, [actor, id] {
                            auto t = actor->tracks.find(id);
                            if (t != actor->tracks.end()) {
                                t->second->discontinuous = true;
                                if (t->second->video)
                                    t->second->video->keyFrame();
                                if (t->second->tileEncoder) t->second->tileEncoder->keyFrame();
                            }
                        });
                    }
                    worker.event(lease, {{"event", "error"},
                                         {"code", "backpressure"},
                                         {"message", "Encoded output queue full; restart at a keyframe"}});
                }
            }
        });
        return true;
    };
    s->thread->start();
    auto *result = s.get();
    sessions.emplace(lease, std::move(s));
    return result;
}
void SessionEngine::callClip(const QString &lease, const QString &method, const QJsonObject &data,
                             const QByteArray &payload, Reply reply) {
    if (method == "play") {
        const double gain = data.value("gain").toDouble(1.0);
        if (!std::isfinite(gain) || gain < 0 || gain > 2) {
            reply({}, {}, "Clip gain must be within 0..2");
            return;
        }
        const auto device = data.value("device").toString();
        static const QRegularExpression hex(QStringLiteral("\\A(?:[0-9a-fA-F]{2})*\\z"));
        if (!hex.match(device).hasMatch()) {
            reply({}, {}, "Invalid output device encoding");
            return;
        }
        const auto audio = parseClipWav(payload);
        if (!audio.ok()) {
            reply({}, {}, "Invalid clip: " + audio.error);
            return;
        }
        bool available = false;
        if (QThread::currentThread() == devices.thread())
            available = devices.outputAvailable(device);
        else
            QMetaObject::invokeMethod(&devices, [&] { available = devices.outputAvailable(device); },
                                      Qt::BlockingQueuedConnection);
        if (!available) {
            reply({}, {}, "Audio output unavailable");
            return;
        }
        stopClip(lease);
        const auto id = QUuid::createUuid().toString(QUuid::Id128);
        QVector<QVector<float>> chunks;
        for (qsizetype offset = 0; offset < audio.samples.size(); offset += FrameSamples) {
            auto chunk = audio.samples.mid(offset, FrameSamples);
            for (auto &sample : chunk) sample = std::clamp(sample * float(gain), -1.f, 1.f);
            chunks.append(std::move(chunk));
        }
        ClipPlayer::Hooks hooks;
        hooks.feed = [this, id, device](QVector<float> chunk, std::function<void(bool)> completed) {
            QMetaObject::invokeMethod(
                &devices,
                [this, id, device, chunk = std::move(chunk), completed = std::move(completed)]() mutable {
                    const bool accepted = devices.play(id, device, std::move(chunk));
                    QMetaObject::invokeMethod(this, [completed = std::move(completed), accepted]() mutable {
                        completed(accepted);
                    });
                },
                Qt::QueuedConnection);
        };
        hooks.query = [this, id, device](std::function<void(const ClipPlayer::DrainState &)> completed) {
            QMetaObject::invokeMethod(
                &devices,
                [this, id, device, completed = std::move(completed)]() mutable {
                    const auto raw = devices.playbackState(device, id);
                    ClipPlayer::DrainState state;
                    state.queuedFrames = raw.queuedFrames;
                    state.pendingBytes = raw.pendingBytes;
                    state.output = raw.output;
                    state.sinkActive = raw.sinkActive;
                    QMetaObject::invokeMethod(this, [completed = std::move(completed), state]() mutable {
                        completed(state);
                    });
                },
                Qt::QueuedConnection);
        };
        hooks.finished = [this, id, lease] { finishClip(id, lease, false, {}); };
        hooks.failed = [this, id, lease](const QString &message) {
            finishClip(id, lease, true, message);
        };
        auto clip = std::make_unique<ClipPlayer>(id, lease, std::move(chunks), std::move(hooks));
        auto *player = clip.get();
        clips[id] = std::move(clip);
        leaseClips[lease] = id;
        player->start();
        reply({{"clipId", id}}, {}, {});
        return;
    }
    if (method == "stop") {
        const auto id = data.value("clipId").toString();
        if (leaseClips.value(lease) == id) stopClip(lease);
        reply({{"clipId", id}}, {}, {});
        return;
    }
    reply({}, {}, "Unsupported clip method");
}

void SessionEngine::finishClip(const QString &clipId, const QString &lease, bool failed,
                               const QString &message) {
    const auto found = clips.find(clipId);
    if (found == clips.end() || found->second->stopped()) return;
    if (failed)
        worker.event(lease, {{"event", "failed"}, {"clip", clipId}, {"message", message}});
    else
        worker.event(lease, {{"event", "finished"}, {"clip", clipId}});
    if (leaseClips.value(lease) == clipId) leaseClips.remove(lease);
    detach(clipId);
    // Keep the player alive until its terminal hook returned; pending feed or
    // drain completions are guarded by QPointer and simply do nothing then.
    QTimer::singleShot(0, this, [this, clipId] {
        const auto remaining = clips.find(clipId);
        if (remaining != clips.end() && remaining->second->terminal()) clips.erase(remaining);
    });
}

void SessionEngine::stopClip(const QString &lease) {
    const auto id = leaseClips.take(lease);
    if (id.isEmpty()) return;
    const auto found = clips.find(id);
    if (found == clips.end()) return;
    found->second->stop();
    detach(id);
    clips.erase(found);
}

void SessionEngine::call(const QString &lease, const QString &service, const QString &method,
                         const QJsonObject &d, const QByteArray &payload, Reply reply) {
    if (service == ClipService) {
        callClip(lease, method, d, payload, reply);
        return;
    }
    auto *s = session(lease);
    if (!s || s->closing) {
        reply({}, {}, "Session unavailable");
        return;
    }
    if (method == "sources") {
        QMetaObject::invokeMethod(&devices, [this, reply] {
            auto sources = devices.sources();
            QMetaObject::invokeMethod(this, [reply, sources] { reply({{"sources", sources}}, {}, {}); });
        });
        return;
    }
    if (method == "capabilities") {
        QJsonArray processors;
        for (const auto &id : extensions.ids())
            processors.append(id);
        reply(
            {{"compiled", QJsonObject{{"ffmpeg", av_version_info()},
                                      {"ffmpegFingerprint", ZTMEDIA_FFMPEG_FINGERPRINT},
                                      {"audio", QJsonArray{"opus"}},
                                      {"video", QJsonArray{"h264","bgra-tiles"}},
                                      {"processors", processors}}},
             {"runtime", QJsonObject{{"softwareH264", avcodec_find_encoder_by_name("libopenh264") != nullptr},
                                      {"softwareTiles", true},
                                     {"hardwareEncoding", false}}},
             {"limits", QJsonObject{{"tracks", 128},
                                    {"workerVideoTracks", 16},
                                    {"width", 1920},
                                    {"height", 1080},
                                    {"packetBytes", double(protocol::service::BulkChannel::MessageBytes)},
                                    {"queueBytes", double(protocol::service::BulkChannel::QueueBytes)},
                                    {"previewSlots", 3}}}},
            {}, {});
        return;
    }
    auto id = d["id"].toString();
    if (method == "track.stop" || method == "track.remove") {
        if (s->operations.contains(id))
            s->operations[id]->store(true);
        s->active.remove(id);
        detach(id);
    }
    auto *actor = s->actor;
    if (method == "track.start") {
        if (s->active.contains(id)) {
            reply({}, {}, {});
            return;
        }
        if (s->operations.contains(id))
            s->operations[id]->store(true);
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        s->operations[id] = cancelled;
        QMetaObject::invokeMethod(actor, [this, lease, actor, id, cancelled,
                                          reply = std::move(reply)]() mutable {
            auto track = actor->tracks.find(id);
            if (track == actor->tracks.end()) {
                QMetaObject::invokeMethod(this, [reply] { reply({}, {}, "Unknown or stale track"); });
                return;
            }
            auto config = track->second->config;
            QMetaObject::invokeMethod(this, [this, lease, actor, id, config, cancelled,
                                             reply = std::move(reply)]() mutable {
                auto found = sessions.find(lease);
                if (found == sessions.end() || found->second->closing || cancelled->load()) {
                    reply({}, {}, "Session closing");
                    return;
                }
                auto queue = found->second->queued;
                DeviceManager::Consumer consumer;
                consumer.audio = [actor, id, queue, cancelled](QByteArray bytes, QAudioFormat format,qint64 capturedAt) {
                    if (cancelled->load())
                        return;
                    if (queue->fetch_add(1) >= 32) {
                        queue->fetch_sub(1);
                        return;
                    }
                    QMetaObject::invokeMethod(
                        actor, [actor, id, queue, cancelled,capturedAt, bytes = std::move(bytes), format] {
                            queue->fetch_sub(1);
                            if (!cancelled->load())
                                actor->audio(id, bytes, format,capturedAt);
                        });
                };
                auto mailbox = std::make_shared<VideoMailbox>();
                QMetaObject::invokeMethod(actor, [actor, id, mailbox] {
                    auto track = actor->tracks.find(id);
                    if (track != actor->tracks.end()) track->second->mailbox = mailbox;
                });
                consumer.video = [actor, id, queue, mailbox, cancelled](QVideoFrame frame) {
                    ++mailbox->offered;
                    if (cancelled->load()) {
                        ++mailbox->cancellations;
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> guard(mailbox->mutex);
                        mailbox->latest = std::move(frame);
                        if (mailbox->queued) {
                            ++mailbox->replaced;
                            ++mailbox->replacements;
                            return;
                        }
                        mailbox->queued = true;
                    }
                    if (queue->fetch_add(1) >= 32) {
                        ++mailbox->queueDrops;
                        queue->fetch_sub(1);
                        std::lock_guard<std::mutex> guard(mailbox->mutex);
                        mailbox->queued = false;
                        return;
                    }
                    QMetaObject::invokeMethod(actor, [actor, id, queue, mailbox, cancelled] {
                        queue->fetch_sub(1);
                        QVideoFrame frame;
                        quint64 replaced = 0;
                        {
                            std::lock_guard<std::mutex> guard(mailbox->mutex);
                            frame = std::move(mailbox->latest);
                            replaced = mailbox->replaced;
                            mailbox->replaced = 0;
                            mailbox->queued = false;
                        }
                        if (cancelled->load()) {
                            ++mailbox->cancellations;
                            return;
                        }
                        ++mailbox->delivered;
                        auto track = actor->tracks.find(id);
                        if (track != actor->tracks.end())
                            track->second->dropped += replaced;
                        actor->video(id, std::move(frame));
                    });
                };
                consumer.failed = [this, lease, id](QString error) {
                    QTimer::singleShot(0, this, [this, lease, id, error] {
                        auto s = sessions.find(lease);
                        if (s == sessions.end() || s->second->closing)
                            return;
                        s->second->active.remove(id);
                        detach(id);
                        auto *a = s->second->actor;
                        QMetaObject::invokeMethod(a, [a, id] {
                            auto t = a->tracks.find(id);
                            if (t != a->tracks.end())
                                t->second->running = false;
                        });
                        worker.event(
                            lease,
                            {{"event", "error"}, {"code", "device"}, {"track", id}, {"message", error}});
                    });
                };
                QMetaObject::invokeMethod(&devices, [this, lease, id, config, actor, cancelled,
                                                     consumer = std::move(consumer),
                                                     reply = std::move(reply)]() mutable {
                    auto error = cancelled->load() ? QString("Track start cancelled")
                                                   : devices.attach(id, config, std::move(consumer));
                    if (cancelled->load())
                        devices.detach(id);
                    QMetaObject::invokeMethod(this, [this, lease, id, actor, error, cancelled,
                                                     reply = std::move(reply)] {
                        auto found = sessions.find(lease);
                        if (found == sessions.end() || found->second->closing || cancelled->load()) {
                            reply({}, {}, "Track start cancelled or session closing");
                            return;
                        }
                        if (!error.isEmpty()) {
                            reply({}, {}, error);
                            return;
                        }
                        found->second->active.insert(id);
                        QMetaObject::invokeMethod(actor, [this, actor, id, reply] {
                            actor->command(
                                "track.start", {{"id", id}},
                                [this, reply](const QJsonObject &d, const QByteArray &p, const QString &e) {
                                    QMetaObject::invokeMethod(this, [reply, d, p, e] { reply(d, p, e); });
                                });
                        });
                    });
                });
            });
        });
        return;
    }
    QMetaObject::invokeMethod(actor, [this, lease, actor, method, d, id, reply = std::move(reply)]() mutable {
        actor->command(method, d,
                       [this, lease, method, id, reply = std::move(reply)](
                           const QJsonObject &result, const QByteArray &bytes, const QString &error) mutable {
                           QMetaObject::invokeMethod(this, [this, lease, method, id, reply = std::move(reply),
                                                            result, bytes, error]() mutable {
                               auto it = sessions.find(lease);
                               if (it == sessions.end() || it->second->closing) {
                                   reply({}, {}, "Session closing");
                                   return;
                               }
                               if (error.isEmpty() && method == "track.create")
                                   it->second->tracks.insert(result["id"].toString());
                               if (error.isEmpty() && method == "track.remove")
                                   it->second->tracks.remove(id);
                               auto response = result;
                               if (method == "statistics") {
                                   response["queueDepth"] = it->second->queued->load();
                                   response["queuePeak"] = it->second->queued->peak.load();
                                   response["queueBytes"] = QString::number(it->second->queued->bytes.load());
                                   response["queuePeakBytes"] =
                                       QString::number(it->second->queued->peakBytes.load());
                               }
                               QMetaObject::invokeMethod(
                                   &devices, [this, response, bytes, error, reply, method]() mutable {
                                       if (method == "statistics")
                                           response["devices"] = devices.statistics();
                                       QMetaObject::invokeMethod(this, [response, bytes, error, reply] {
                                           reply(response, bytes, error);
                                       });
                                   });
                           });
                       });
    });
}
void SessionEngine::submit(const QString &lease, const QJsonObject &h, const QByteArray &p) {
    auto it = sessions.find(lease);
    if (it == sessions.end() || it->second->closing)
        return;
    auto queue = it->second->queued;
    if (!queue->reserve(p.size())) {
        worker.event(
            lease, {{"event", "error"}, {"code", "backpressure"}, {"message", "Input byte budget exceeded"}});
        return;
    }
    if (queue->fetch_add(1) >= 32) {
        queue->fetch_sub(1);
        queue->release(p.size());
        worker.event(lease, {{"event", "error"}, {"code", "backpressure"}, {"message", "Input queue full"}});
        return;
    }
    auto *actor = it->second->actor;
    QMetaObject::invokeMethod(actor, [actor, queue, h, p] {
        queue->fetch_sub(1);
        queue->release(p.size());
        actor->submit(h, p);
    });
}
void SessionEngine::release(const QString &lease, Reply reply) {
    stopClip(lease);
    auto it = sessions.find(lease);
    if (it == sessions.end()) {
        reply({}, {}, {});
        return;
    }
    auto *s = it->second.get();
    s->closing = true;
    for (const auto &cancel : s->operations)
        cancel->store(true);
    for (const auto &id : s->tracks)
        detach(id);
    auto *actor = s->actor;
    connect(s->thread.get(), &QThread::finished, this, [this, lease, reply] {
        QMetaObject::invokeMethod(&devices, [this, lease, reply] {
            QMetaObject::invokeMethod(this, [this, lease, reply] {
                sessions.erase(lease);
                reply({}, {}, {});
            });
        });
    });
    // Unbind producers before destroying their callback target. The device
    // event queue is a barrier for all preceding detach and readyRead callbacks.
    QMetaObject::invokeMethod(&devices, [actor] {
        QMetaObject::invokeMethod(actor, [actor] {
            actor->timer->stop();
            actor->tracks.clear();
            QThread::currentThread()->quit();
        });
    });
}
void SessionEngine::shutdown() {
    for (auto &entry : clips) {
        if (entry.second) entry.second->stop();
        detach(entry.first);
    }
    clips.clear();
    leaseClips.clear();
    for (auto &s : sessions) {
        s.second->closing = true;
        for (const auto &cancel : s.second->operations)
            cancel->store(true);
    }
    auto releaseDevices = [this] {
        devices.changed = {};
        for (const auto &entry : sessions)
            for (const auto &id : entry.second->tracks)
                devices.detach(id);
    };
    if (devices.thread() == QThread::currentThread())
        releaseDevices();
    else if (devices.thread()->isRunning())
        QMetaObject::invokeMethod(&devices, releaseDevices, Qt::BlockingQueuedConnection);
    for (auto &entry : sessions) {
        auto &s = *entry.second;
        if (s.thread->isRunning()) {
            auto *actor = s.actor;
            QMetaObject::invokeMethod(actor, [actor] {
                actor->timer->stop();
                actor->tracks.clear();
                QThread::currentThread()->quit();
            });
            s.thread->wait();
        }
    }
    sessions.clear();
}
} // namespace vusiif::zto::media::engine
