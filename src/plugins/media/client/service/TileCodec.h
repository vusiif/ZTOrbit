#pragma once
#include <ztmedia/MediaTypes.h>
#include <QBitArray>
#include <QImage>
#include <QMap>

namespace vusiif::zto::media::engine {
// Absolute, independently decodable 64x64 BGRA tiles. No inter-tile references.
class TileEncoder {
public:
    static constexpr int Edge = 64, HeaderBytes = 40, PacketBytes = 256 * 1024;
    explicit TileEncoder(const TrackConfig &config);
    void capture(const QImage &image, qint64 pts);
    void stop();
    // Never abandon an in-flight sweep. Remember one coalesced follow-up for
    // receivers that became ready after its first chunk; silently ignoring
    // their request leaves the initial unknown regions missing until recovery.
    void keyFrame() {
        if (sendingBaseline) refreshPending = true;
        else resetPending = true;
    }
    QList<EncodedPacket> drain(qint64 now);
    QJsonObject statistics() const;
private:
    TrackConfig config;
    QImage latest, baseline, sent;
    QBitArray dirty;
    quint64 epoch = 0, epochStart = 0, sequence = 0, tilesSent = 0, bytesSent = 0;
    quint64 unchanged = 0, deferred = 0, baselines = 0;
    qint64 latestPts = 0, baselinePts = 0, refillAt = -1, heartbeatAt = 0, packetAt = 0;
    double tokens = 0, capacity = 0, rate = 0;
    int columns = 0, rows = 0, cursor = 0;
    bool resetPending = true, sendingBaseline = false, refreshPending = false;
    void findChanges();
};

class TileDecoder {
public:
    explicit TileDecoder(const TrackConfig &config) : config(config) {}
    QList<QPair<QImage, qint64>> decode(const EncodedPacket &packet, qint64 now, QString &error);
    bool refreshRequired(qint64 now) const;
    QJsonObject statistics() const;
private:
    TrackConfig config;
    QImage canvas;
    QBitArray covered;
    QVector<quint64> versions;
    QMap<quint64, qint64> missing;
    quint64 epoch = 0, epochStart = 0, nextSequence = 0, applied = 0, stale = 0, rejected = 0;
    int coverage = 0;
    qint64 progressAt = 0;
    bool forceRefresh = false;
};
} // namespace vusiif::zto::media::engine
