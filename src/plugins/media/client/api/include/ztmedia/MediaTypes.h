#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

namespace vusiif::zto::media {
inline constexpr auto SessionService = "media.session";
inline constexpr auto SessionApi = "1";
inline constexpr auto ClipService = "media.clip";
inline constexpr auto ClipApi = "1";
inline constexpr int FrameSamples = 960;
inline constexpr int MaxOpusBytes = 1170;
inline constexpr int ClipSampleRate = 48000;
inline constexpr int MaxClipBytes = 256 * 1024;
inline constexpr int MaxClipDurationMs = 2000;
struct MediaError {
    QString code, message;
    bool ok() const { return code.isEmpty(); }
};
template <class T> struct Result {
    T value{};
    MediaError error;
    bool ok() const { return error.ok(); }
};
template <class T> using Completion = std::function<void(Result<T>)>;
struct TimeBase {
    int numerator = 1, denominator = 1000000;
};
struct AudioFormat {
    int sampleRate = 48000, channels = 1;
    QString sampleFormat = "f32";
};
struct VideoFormat {
    int width = 1280, height = 720;
    QString pixelFormat = "bgra", colorSpace = "bt709", colorRange = "limited";
};
struct SourceDescriptor {
    QString id, kind, description;
    bool isDefault = false;
};
struct SourceSnapshot {
    bool ready = false;
    QList<SourceDescriptor> sources;
};
struct CodecConfig {
    QString codec = "opus";
    int bitrate = 24000, frameRate = 30, keyFrameInterval = 30;
    bool allowAdjustment = false, preferHardware = false;
    QByteArray initializationData;
};
struct TrackConfig {
    QString kind = "audio", direction = "send", source, output;
    AudioFormat audio;
    VideoFormat video;
    CodecConfig codec;
    QStringList processors;
    QString syncGroup;
    bool recordAllowed = true, preview = false;
    float gain = 1.0f;
};
struct TrackDescriptor {
    QString id;
    quint64 generation = 0, configuration = 1;
    TrackConfig config;
};
struct EncodedPacket {
    QString track, codec;
    quint64 generation = 0, configuration = 1, sequence = 0;
    qint64 pts = 0, duration = 20000;
    std::optional<qint64> dts;
    TimeBase timeBase;
    bool keyFrame = false, discontinuity = false;
    QByteArray initializationData, data;
};
struct FramePlane {
    qsizetype offset = 0, size = 0;
    int stride = 0;
};
// Borrowed only for the duration of the frame callback. Copy data to retain it.
struct FrameView {
    QString track;
    quint64 generation = 0, sequence = 0, configuration = 1;
    qint64 pts = 0, duration = 0;
    TimeBase timeBase;
    VideoFormat video{1280, 720, "bgra", "bt709", "full"};
    AudioFormat audio;
    QList<FramePlane> planes;
    const uchar *data = nullptr;
    qsizetype size = 0;
    bool decoded = false;
    QByteArray copy() const { return QByteArray(reinterpret_cast<const char *>(data), size); }
};
struct Capabilities {
    QJsonObject compiled, runtime, limits;
};
struct Statistics {
    QJsonObject values;
    // Returns an owning JSON snapshot, or an empty object for an unknown track.
    QJsonObject track(const QString &id) const {
        for (const auto &entry : values.value("tracks").toArray()) {
            const auto snapshot = entry.toObject();
            if (snapshot.value("id").toString() == id)
                return snapshot;
        }
        return {};
    }
};
inline QJsonObject toJson(const TrackConfig &c) {
    QJsonArray processors;
    for (const auto &p : c.processors)
        processors.append(p);
    return {{"kind", c.kind},
            {"direction", c.direction},
            {"source", c.source},
            {"output", c.output},
            {"audio", QJsonObject{{"sampleRate", c.audio.sampleRate},
                                  {"channels", c.audio.channels},
                                  {"sampleFormat", c.audio.sampleFormat}}},
            {"video", QJsonObject{{"width", c.video.width},
                                  {"height", c.video.height},
                                  {"pixelFormat", c.video.pixelFormat},
                                  {"colorSpace", c.video.colorSpace},
                                  {"colorRange", c.video.colorRange}}},
            {"codec",
             QJsonObject{{"name", c.codec.codec},
                         {"bitrate", c.codec.bitrate},
                         {"frameRate", c.codec.frameRate},
                         {"keyFrameInterval", c.codec.keyFrameInterval},
                         {"allowAdjustment", c.codec.allowAdjustment},
                         {"preferHardware", c.codec.preferHardware},
                         {"initializationData", QString::fromLatin1(c.codec.initializationData.toHex())}}},
            {"processors", processors},
            {"syncGroup", c.syncGroup},
            {"recordAllowed", c.recordAllowed},
            {"preview", c.preview},
            {"gain", double(c.gain)}};
}
inline TrackConfig trackConfig(const QJsonObject &o) {
    TrackConfig c;
    c.kind = o["kind"].toString("audio");
    c.direction = o["direction"].toString("send");
    c.source = o["source"].toString();
    c.output = o["output"].toString();
    auto a = o["audio"].toObject();
    c.audio = {a["sampleRate"].toInt(48000), a["channels"].toInt(1), a["sampleFormat"].toString("f32")};
    auto v = o["video"].toObject();
    c.video = {v["width"].toInt(1280), v["height"].toInt(720), v["pixelFormat"].toString("bgra"),
               v["colorSpace"].toString("bt709"), v["colorRange"].toString("limited")};
    auto e = o["codec"].toObject();
    c.codec.codec = e["name"].toString("opus");
    c.codec.bitrate = e["bitrate"].toInt(24000);
    c.codec.frameRate = e["frameRate"].toInt(30);
    c.codec.keyFrameInterval = e["keyFrameInterval"].toInt(30);
    c.codec.allowAdjustment = e["allowAdjustment"].toBool();
    c.codec.preferHardware = e["preferHardware"].toBool();
    c.codec.initializationData = QByteArray::fromHex(e["initializationData"].toString().toLatin1());
    for (const auto &p : o["processors"].toArray())
        c.processors.append(p.toString());
    c.syncGroup = o["syncGroup"].toString();
    c.recordAllowed = o["recordAllowed"].toBool(true);
    c.preview = o["preview"].toBool();
    c.gain = float(o["gain"].toDouble(1));
    return c;
}
inline QJsonObject toJson(const EncodedPacket &p) {
    QJsonObject o{{"track", p.track},
                  {"codec", p.codec},
                  {"generation", QString::number(p.generation)},
                  {"configuration", QString::number(p.configuration)},
                  {"sequence", QString::number(p.sequence)},
                  {"pts", QString::number(p.pts)},
                  {"duration", QString::number(p.duration)},
                  {"timeNumerator", p.timeBase.numerator},
                  {"timeDenominator", p.timeBase.denominator},
                  {"keyFrame", p.keyFrame},
                  {"discontinuity", p.discontinuity},
                  {"initializationData", QString::fromLatin1(p.initializationData.toHex())}};
    if (p.dts)
        o["dts"] = QString::number(*p.dts);
    return o;
}
inline EncodedPacket encodedPacket(const QJsonObject &o, const QByteArray &data) {
    EncodedPacket p;
    p.track = o["track"].toString();
    p.codec = o["codec"].toString();
    p.generation = o["generation"].toString().toULongLong();
    p.configuration = o["configuration"].toString().toULongLong();
    p.sequence = o["sequence"].toString().toULongLong();
    p.pts = o["pts"].toString().toLongLong();
    p.duration = o["duration"].toString().toLongLong();
    p.timeBase = {o["timeNumerator"].toInt(1), o["timeDenominator"].toInt(1000000)};
    if (o.contains("dts"))
        p.dts = o["dts"].toString().toLongLong();
    p.keyFrame = o["keyFrame"].toBool();
    p.discontinuity = o["discontinuity"].toBool();
    p.initializationData = QByteArray::fromHex(o["initializationData"].toString().toLatin1());
    p.data = data;
    return p;
}
} // namespace vusiif::zto::media
