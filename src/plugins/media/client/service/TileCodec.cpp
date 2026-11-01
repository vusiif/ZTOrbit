#include "TileCodec.h"
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <limits>
#include <zlib.h>

namespace vusiif::zto::media::engine {
namespace {
constexpr quint8 Baseline = 1, Heartbeat = 2;
template<class T> void append(QByteArray &out, T value) {
    const auto size = out.size(); out.resize(size + sizeof(T));
    qToBigEndian(value, reinterpret_cast<uchar *>(out.data() + size));
}
template<class T> T read(const QByteArray &data, int at) {
    return qFromBigEndian<T>(reinterpret_cast<const uchar *>(data.constData() + at));
}
QRect rect(int index, int columns, const QSize &size) {
    const int x = index % columns * TileEncoder::Edge, y = index / columns * TileEncoder::Edge;
    return {x, y, std::min(TileEncoder::Edge, size.width() - x),
                  std::min(TileEncoder::Edge, size.height() - y)};
}
QByteArray pixels(const QImage &image, const QRect &area) {
    QByteArray out(area.width() * area.height() * 4, Qt::Uninitialized);
    for (int row = 0; row < area.height(); ++row)
        std::memcpy(out.data() + row * area.width() * 4,
                    image.constScanLine(area.y() + row) + area.x() * 4, area.width() * 4);
    return out;
}
void put(QImage &image, const QRect &area, const QByteArray &raw) {
    for (int row = 0; row < area.height(); ++row)
        std::memcpy(image.scanLine(area.y() + row) + area.x() * 4,
                    raw.constData() + row * area.width() * 4, area.width() * 4);
}
bool equal(const QImage &a, const QImage &b, const QRect &area) {
    for (int row = 0; row < area.height(); ++row)
        if (std::memcmp(a.constScanLine(area.y() + row) + area.x() * 4,
                        b.constScanLine(area.y() + row) + area.x() * 4, area.width() * 4)) return false;
    return true;
}
// Pixel PackBits: each token encodes 1..128 literal or repeated BGRA pixels.
// A raw fallback bounds encoded size; decoding never grows beyond one tile.
QByteArray pack(const QByteArray &raw) {
    QByteArray out; out.reserve(raw.size());
    const int count = int(raw.size() / 4);
    const auto same = [&](int a, int b) { return !std::memcmp(raw.constData()+a*4,raw.constData()+b*4,4); };
    int at = 0;
    while (at < count) {
        int run = 1;
        while (run < 128 && at + run < count && same(at, at + run)) ++run;
        if (run >= 2) {
            out.append(char(0x80 | (run - 1))); out.append(raw.constData()+at*4, 4); at += run;
        } else {
            const int begin = at++;
            while (at < count && at-begin < 128 && !(at+1 < count && same(at,at+1))) ++at;
            out.append(char(at-begin-1)); out.append(raw.constData()+begin*4,(at-begin)*4);
        }
        if (out.size() >= raw.size()) return {};
    }
    return out;
}
QByteArray unpack(const QByteArray &packed, int expected) {
    QByteArray raw; raw.reserve(expected);
    int at = 0;
    while (at < packed.size()) {
        const auto token = quint8(packed[at++]); const int count = (token & 127) + 1;
        const int bytes = (token & 128) ? 4 : count * 4;
        if (at + bytes > packed.size() || raw.size() + count * 4 > expected) return {};
        if (token & 128) for (int i=0;i<count;++i) raw.append(packed.constData()+at,4);
        else raw.append(packed.constData()+at,bytes);
        at += bytes;
    }
    return raw.size() == expected ? raw : QByteArray{};
}
// Inflate into exactly one tile. Do not use qUncompress on network input: its
// advertised length is untrusted and may cause allocation beyond a tile.
QByteArray inflateTile(const QByteArray &packed, int expected) {
    QByteArray raw(expected, Qt::Uninitialized);
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) return {};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(packed.constData()));
    stream.avail_in = uInt(packed.size());
    stream.next_out = reinterpret_cast<Bytef *>(raw.data());
    stream.avail_out = uInt(expected);
    const int status = inflate(&stream, Z_FINISH);
    const bool valid = status == Z_STREAM_END && stream.total_out == uLong(expected)
                       && stream.avail_in == 0;
    inflateEnd(&stream);
    return valid ? raw : QByteArray{};
}
QByteArray header(const QSize &size, quint8 flags, quint16 count, quint64 epoch, quint64 start,quint64 sequence) {
    QByteArray out("ZTD1",4); append(out,quint8(1)); append(out,flags);
    append(out,quint16(TileEncoder::Edge)); append(out,quint16(size.width()));
    append(out,quint16(size.height())); append(out,count); append(out,quint16(0));
    append(out,epoch); append(out,start); append(out,sequence); return out;
}
}

TileEncoder::TileEncoder(const TrackConfig &value) : config(value) {
    columns = (config.video.width + Edge - 1) / Edge;
    rows = (config.video.height + Edge - 1) / Edge;
    rate = config.codec.bitrate / 8.0;
    capacity = std::clamp(rate / 4, 32768.0, double(PacketBytes)); tokens = capacity;
}
void TileEncoder::findChanges() {
    if (latest.isNull() || sendingBaseline) return;
    for (int index = 0; index < dirty.size(); ++index) {
        const bool changed = !equal(latest,sent,rect(index,columns,latest.size()));
        dirty.setBit(index,changed);
        if (!changed) ++unchanged;
    }
}
void TileEncoder::capture(const QImage &image, qint64 pts) {
    latest = image; latestPts = pts;
    if (!sent.isNull()) findChanges();
}
void TileEncoder::stop() {
    latest={};baseline={};sent={};dirty={};cursor=0;
    sendingBaseline=false;resetPending=true;refreshPending=false;
    // Retain epoch/sequence and token history. A still-running receiver must
    // recognize the next baseline as newer; stop/start must not refill budget.
}
QList<EncodedPacket> TileEncoder::drain(qint64 now) {
    if (latest.isNull() || now < packetAt) return {};
    if (refillAt >= 0) tokens = std::min(capacity,tokens + std::max(qint64(0),now-refillAt)*rate/1000000);
    refillAt = now;
    if (resetPending) {
        ++epoch; ++baselines; epochStart = sequence; resetPending = false;
        baseline = latest; baselinePts = latestPts; sendingBaseline = true;
        sent = QImage(latest.size(),QImage::Format_ARGB32); sent.fill(Qt::black);
        dirty = QBitArray(columns*rows,true); cursor = 0;
    }
    const bool wasBaseline = sendingBaseline;
    const auto &source = sendingBaseline ? baseline : latest;
    QByteArray body; quint16 count = 0;
    int scanned = 0;
    while (scanned < dirty.size() && tokens >= HeaderBytes + 8) {
        const int index = (cursor + scanned) % dirty.size();
        if (!dirty.testBit(index)) { ++scanned; continue; }
        const auto area = rect(index,columns,source.size());
        const auto raw = pixels(source,area); auto compressed = pack(raw);
        quint16 encoding = compressed.isEmpty() ? 0 : 1;
        if (!encoding) compressed = raw;
        // PackBits only sees adjacent equal pixels. Text edges and scaled
        // desktop patterns repeat across rows, so their first screen used many
        // fragmented packets even though a bounded lossless deflate is small.
        // Keep raw/PackBits when they are smaller; all modes remain absolute.
        const auto deflated = qCompress(raw, 1).mid(4);
        if (!deflated.isEmpty() && deflated.size() < compressed.size()) {
            compressed = deflated;
            encoding = 2;
        }
        const auto bytes = 8 + compressed.size();
        // A baseline spans packets. Keep each encoder output small enough to
        // avoid a 100+ datagram UDP burst when forwarded to several viewers.
        // PacketBytes remains the decoder/protocol ceiling, not a burst goal.
        constexpr int EncodePacketBytes = 8 * 1024;
        // One incompressible tile still fits (16 KiB plus headers). Never let a
        // low burst target starve that tile, and never combine two such tiles
        // into a fan-out burst that consumes the entire reassembly deadline.
        const int packetLimit = std::max(EncodePacketBytes, HeaderBytes + int(bytes));
        if (HeaderBytes + body.size() + bytes > std::min(double(packetLimit),tokens)) {
            ++deferred; break;
        }
        append(body,quint16(index)); append(body,encoding); append(body,quint32(compressed.size()));
        body.append(compressed); put(sent,area,raw); dirty.clearBit(index); ++count; ++tilesSent;
        ++scanned;
    }
    // The next pending block starts the next budget interval, preventing a
    // continuously changing region from starving the rest of the canvas.
    cursor = (cursor + scanned) % dirty.size();
    if (!count && (now < heartbeatAt || tokens < HeaderBytes)) return {};
    const auto pts = wasBaseline ? baselinePts : latestPts;
    EncodedPacket packet; packet.codec = "bgra-tiles"; packet.sequence = sequence++;
    packet.pts = pts; packet.duration = 1000000 / config.codec.frameRate;
    packet.keyFrame = wasBaseline && count;
    packet.data = header(latest.size(), count ? (wasBaseline ? Baseline : 0) : Heartbeat,
                         count,epoch,epochStart,packet.sequence) + body;
    tokens -= packet.data.size(); bytesSent += packet.data.size(); heartbeatAt = now + 500000;
    // Advance from the previous deadline rather than resetting to now, so a
    // capture stream slightly faster than the configured rate cannot push the
    // output rate below the target by repeatedly restarting the interval.
    packetAt += 1000000 / config.codec.frameRate;
    if (packetAt < now) packetAt = now + 1000000 / config.codec.frameRate;
    if (sendingBaseline && dirty.count(true) == 0) {
        sendingBaseline = false; baseline = {};
        if (refreshPending) { resetPending = true; refreshPending = false; }
        findChanges();
    }
    return {std::move(packet)};
}
QJsonObject TileEncoder::statistics() const {
    return {{"tilesSent",QString::number(tilesSent)},{"tileBytesSent",QString::number(bytesSent)},
            {"tileEpoch",QString::number(epoch)},{"tileEpochStart",QString::number(epochStart)},
            {"tileNextSequence",QString::number(sequence)},{"tileBaselineInFlight",sendingBaseline},
            {"tileRefreshPending",refreshPending || resetPending},
            {"tileLatestPts",QString::number(latestPts)},{"tileBaselinePts",QString::number(baselinePts)},
            {"unchangedTiles",QString::number(unchanged)},{"tileBudgetDeferrals",QString::number(deferred)},
            {"tilePending",dirty.count(true)},{"tileBaselines",QString::number(baselines)},
            {"tileBurstBytes",int(capacity)}};
}

QList<QPair<QImage,qint64>> TileDecoder::decode(const EncodedPacket &packet,qint64 now,QString &error) {
    const auto &data = packet.data;
    const auto reject = [&]() -> QList<QPair<QImage,qint64>> {
        error = "Invalid or oversized tile packet"; ++rejected; forceRefresh = true; return {};
    };
    if (data.size() < TileEncoder::HeaderBytes || data.size() > TileEncoder::PacketBytes ||
        data.first(4) != "ZTD1" || read<quint8>(data,4) != 1 ||
        (read<quint8>(data,5) & ~3) || read<quint16>(data,6) != TileEncoder::Edge ||
        read<quint16>(data,8) != config.video.width || read<quint16>(data,10) != config.video.height ||
        read<quint16>(data,14) != 0) return reject();
    const auto incomingEpoch = read<quint64>(data,16), start = read<quint64>(data,24);
    // Codec ordering is self-contained. Network transports may assign their
    // own packet sequence after negotiation or queue drops.
    const auto codecSequence = read<quint64>(data,32);
    const int count = read<quint16>(data,12);
    const int columns = (config.video.width+63)/64, total = columns*((config.video.height+63)/64);
    const auto flags = read<quint8>(data,5);
    if (packet.keyFrame != bool(flags & Baseline)) return reject();
    if (!incomingEpoch || codecSequence == std::numeric_limits<quint64>::max() || start > codecSequence || count > total ||
        ((flags & Heartbeat) ? (count != 0 || (flags & Baseline)) : count == 0)) return reject();
    if (incomingEpoch < epoch) { ++stale; return {}; }
    QVector<QPair<int,QByteArray>> records; QBitArray seen(total);
    int at = TileEncoder::HeaderBytes;
    for (int record = 0; record < count; ++record) {
        if (at + 8 > data.size()) return reject();
        const int index = read<quint16>(data,at), encoding = read<quint16>(data,at+2);
        const auto bytes = read<quint32>(data,at+4); at += 8;
        if (index >= total || seen.testBit(index) || encoding > 2 ||
            bytes > 64*64*4 || bytes > quint32(data.size()-at)) return reject();
        const auto area = rect(index,columns,QSize(config.video.width,config.video.height));
        const int expected = area.width()*area.height()*4;
        const auto encoded = data.mid(at,bytes); at += bytes;
        auto raw = encoding == 2 ? inflateTile(encoded,expected)
                                : encoding == 1 ? unpack(encoded,expected) : encoded;
        if (raw.size() != expected) return reject();
        records.append({index,std::move(raw)}); seen.setBit(index);
    }
    if (at != data.size()) return reject();
    if (incomingEpoch > epoch) {
        if (!canvas.isNull() && start < nextSequence) return reject();
        epoch = incomingEpoch; epochStart = start; nextSequence = start;
        // A refresh is another sweep of absolute pixels in this track, not a
        // new source. Keep verified tiles across lossy sweeps so one missing
        // chunk cannot reset first-screen progress forever. A new track/stream
        // creates a new decoder and starts with no known pixels.
        if (canvas.isNull()) {
            canvas = QImage(config.video.width,config.video.height,QImage::Format_ARGB32); canvas.fill(Qt::black);
            covered = QBitArray(total); versions = QVector<quint64>(total); coverage = 0;
        }
        missing.clear(); forceRefresh = false; progressAt = now;
    } else if (start != epochStart) return reject();
    missing.remove(codecSequence);
    if (codecSequence >= nextSequence) {
        if (codecSequence-nextSequence > quint64(1024-missing.size())) { missing.clear(); forceRefresh = true; }
        else for (auto seq=nextSequence;seq<codecSequence;++seq) missing.insert(seq,now);
        nextSequence = codecSequence + 1;
    }
    if (packet.discontinuity && !(flags & Baseline)) forceRefresh = true;
    bool changed = false;
    for (const auto &record : records) {
        const int index = record.first;
        if (covered.testBit(index) && versions[index] >= codecSequence) { ++stale; continue; }
        put(canvas,rect(index,columns,canvas.size()),record.second); versions[index] = codecSequence;
        if (!covered.testBit(index)) { covered.setBit(index); ++coverage; }
        ++applied; changed = true; progressAt = now;
    }
    if (changed && coverage == total) return {{canvas,packet.pts}};
    return {};
}
bool TileDecoder::refreshRequired(qint64 now) const {
    if (forceRefresh) return true;
    for (const auto &since : missing) if (now-since >= 200000) return true;
    // No partial baseline is displayed. Heartbeats expose a lost final chunk
    // even when the desktop stops changing immediately afterwards.
    const auto timeout = std::max(qint64(1000000),qint64(64*64*4)*16000000/config.codec.bitrate);
    return !canvas.isNull() && coverage < covered.size() && now-progressAt >= timeout;
}
QJsonObject TileDecoder::statistics() const {
    return {{"tilesApplied",QString::number(applied)},{"tileStale",QString::number(stale)},
            {"tileEpoch",QString::number(epoch)},{"tileEpochStart",QString::number(epochStart)},
            {"tileNextSequence",QString::number(nextSequence)},{"tileForcedRefresh",forceRefresh},
            {"tileRejected",QString::number(rejected)},{"tileCoverage",coverage},
            {"tileCount",covered.size()},{"tileMissingPackets",missing.size()}};
}
} // namespace vusiif::zto::media::engine
