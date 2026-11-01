#include <desktop-share/MediaProtocol.h>

#include <desktop-share/ControlProtocol.h>

#include <ztprotocol/WireProtocol.h>

#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace vusiif::zto::desktop_share {
namespace {

constexpr quint16 KnownFlags = quint16(MediaFlag::KeyFrame)
    | quint16(MediaFlag::Discontinuity) | quint16(MediaFlag::HasDts);

void put16(QByteArray &output, quint16 value)
{
    output.append(char((value >> 8) & 0xFF));
    output.append(char(value & 0xFF));
}

void put32(QByteArray &output, quint32 value)
{
    output.append(char((value >> 24) & 0xFF));
    output.append(char((value >> 16) & 0xFF));
    output.append(char((value >> 8) & 0xFF));
    output.append(char(value & 0xFF));
}

void put64(QByteArray &output, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        output.append(char((value >> shift) & 0xFF));
}

quint16 get16(const uchar *data) { return qFromBigEndian<quint16>(data); }
quint32 get32(const uchar *data) { return qFromBigEndian<quint32>(data); }
quint64 get64(const uchar *data) { return qFromBigEndian<quint64>(data); }

bool tokenIsSet(const std::array<quint8, 16> &token)
{
    return std::any_of(token.begin(), token.end(), [](quint8 byte) { return byte != 0; });
}

QByteArray encodeHeader(const MediaHeader &header)
{
    QByteArray output;
    output.reserve(MediaHeaderBytes);
    output.append(MediaMagic, 4);
    output.append(char(MediaVersion));
    output.append(char(header.type));
    put16(output, header.flags);
    output.append(reinterpret_cast<const char *>(header.token.data()), 16);
    put64(output, header.sessionId);
    put64(output, header.roomId);
    put64(output, header.streamEpoch);
    put64(output, header.sequence);
    put64(output, quint64(header.pts));
    put64(output, quint64(header.dts));
    put32(output, header.duration);
    put32(output, header.packetBytes);
    put16(output, header.fragmentIndex);
    put16(output, header.fragmentCount);
    put16(output, header.fragmentBytes);
    put16(output, 0); // reserved
    Q_ASSERT(output.size() == MediaHeaderBytes);
    return output;
}

quint32 expectedFragmentCount(quint32 packetBytes)
{
    return (packetBytes + quint32(MaxFragmentBytes) - 1) / quint32(MaxFragmentBytes);
}

quint32 expectedFragmentBytes(quint32 packetBytes, quint16 index)
{
    const quint64 consumed = quint64(index) * MaxFragmentBytes;
    if (consumed >= packetBytes) return 0;
    return quint32(std::min<quint64>(MaxFragmentBytes, packetBytes - consumed));
}

void setError(MediaError *error, QString *message, MediaError value, const QString &text)
{
    if (error) *error = value;
    if (message) *message = text;
}

bool validVideoHeader(const MediaHeader &header, MediaError *error, QString *message)
{
    if (header.type != MediaType::Video) {
        setError(error, message, MediaError::BadType, QStringLiteral("不是视频类型"));
        return false;
    }
    if ((header.flags & ~KnownFlags) != 0) {
        setError(error, message, MediaError::BadFlags, QStringLiteral("flags 含未知位"));
        return false;
    }
    if (!tokenIsSet(header.token)) {
        setError(error, message, MediaError::BadToken, QStringLiteral("媒体 token 不能全为零"));
        return false;
    }
    if (header.sessionId == 0) {
        setError(error, message, MediaError::BadSession, QStringLiteral("会话 ID 不能为零"));
        return false;
    }
    if (header.roomId == 0) {
        setError(error, message, MediaError::BadRoom, QStringLiteral("房间 ID 不能为零"));
        return false;
    }
    if (header.streamEpoch == 0) {
        setError(error, message, MediaError::BadEpoch, QStringLiteral("流代次不能为零"));
        return false;
    }
    if (header.packetBytes == 0 || header.packetBytes > quint32(MaxVideoPacketBytes)) {
        setError(error, message, MediaError::BadPacketBytes,
                 QStringLiteral("编码包大小必须在 1..1048576 字节"));
        return false;
    }
    const quint32 count = expectedFragmentCount(header.packetBytes);
    if (header.fragmentCount != count || header.fragmentCount == 0
        || header.fragmentCount > MaxFragmentsPerPacket) {
        setError(error, message, MediaError::BadFragmentCount,
                 QStringLiteral("片数与编码包大小不一致"));
        return false;
    }
    if (header.fragmentIndex >= header.fragmentCount) {
        setError(error, message, MediaError::BadFragmentIndex,
                 QStringLiteral("片索引超出片数"));
        return false;
    }
    const quint32 expected = expectedFragmentBytes(header.packetBytes, header.fragmentIndex);
    if (header.fragmentBytes != expected) {
        setError(error, message, MediaError::BadFragmentBytes,
                 QStringLiteral("片长度与编码包大小和片索引不一致"));
        return false;
    }
    if (!header.hasDts() && header.dts != 0) {
        setError(error, message, MediaError::BadTimestamp,
                 QStringLiteral("未设置 DTS 标志时 dts 必须为零"));
        return false;
    }
    return true;
}

bool validRegisterHeader(const MediaHeader &header, MediaError *error, QString *message)
{
    if (!tokenIsSet(header.token)) {
        setError(error, message, MediaError::BadToken, QStringLiteral("媒体 token 不能全为零"));
        return false;
    }
    if (header.sessionId == 0) {
        setError(error, message, MediaError::BadSession, QStringLiteral("会话 ID 不能为零"));
        return false;
    }
    if (header.roomId == 0) {
        setError(error, message, MediaError::BadRoom, QStringLiteral("房间 ID 不能为零"));
        return false;
    }
    if (header.flags != 0 || header.streamEpoch != 0 || header.sequence != 0 || header.pts != 0
        || header.dts != 0 || header.duration != 0 || header.packetBytes != 0
        || header.fragmentIndex != 0 || header.fragmentCount != 0 || header.fragmentBytes != 0) {
        setError(error, message, MediaError::RegisterFields,
                 QStringLiteral("注册包除 token、会话和房间外必须为零"));
        return false;
    }
    return true;
}

} // namespace

QString mediaErrorName(MediaError error)
{
    switch (error) {
    case MediaError::None: return QStringLiteral("NONE");
    case MediaError::TooShort: return QStringLiteral("TOO_SHORT");
    case MediaError::TooLong: return QStringLiteral("TOO_LONG");
    case MediaError::BadMagic: return QStringLiteral("BAD_MAGIC");
    case MediaError::BadVersion: return QStringLiteral("BAD_VERSION");
    case MediaError::BadType: return QStringLiteral("BAD_TYPE");
    case MediaError::BadFlags: return QStringLiteral("BAD_FLAGS");
    case MediaError::BadReserved: return QStringLiteral("BAD_RESERVED");
    case MediaError::BadToken: return QStringLiteral("BAD_TOKEN");
    case MediaError::BadSession: return QStringLiteral("BAD_SESSION");
    case MediaError::BadRoom: return QStringLiteral("BAD_ROOM");
    case MediaError::BadEpoch: return QStringLiteral("BAD_EPOCH");
    case MediaError::BadPacketBytes: return QStringLiteral("BAD_PACKET_BYTES");
    case MediaError::BadFragmentCount: return QStringLiteral("BAD_FRAGMENT_COUNT");
    case MediaError::BadFragmentIndex: return QStringLiteral("BAD_FRAGMENT_INDEX");
    case MediaError::BadFragmentBytes: return QStringLiteral("BAD_FRAGMENT_BYTES");
    case MediaError::BadLength: return QStringLiteral("BAD_LENGTH");
    case MediaError::BadTimestamp: return QStringLiteral("BAD_TIMESTAMP");
    case MediaError::RegisterFields: return QStringLiteral("REGISTER_FIELDS");
    case MediaError::BadRoute: return QStringLiteral("BAD_ROUTE");
    case MediaError::Overflow: return QStringLiteral("OVERFLOW");
    }
    return QString();
}

QByteArray encodeRegister(const std::array<quint8, 16> &token, quint64 sessionId, quint64 roomId)
{
    MediaHeader header;
    header.type = MediaType::Register;
    header.token = token;
    header.sessionId = sessionId;
    header.roomId = roomId;
    MediaError error = MediaError::None;
    QString message;
    if (!validRegisterHeader(header, &error, &message)) return {};
    return encodeHeader(header);
}

QByteArray encodeRegisterAck(const std::array<quint8, 16> &token, quint64 sessionId, quint64 roomId)
{
    MediaHeader header;
    header.type = MediaType::RegisterAck;
    header.token = token;
    header.sessionId = sessionId;
    header.roomId = roomId;
    MediaError error = MediaError::None;
    QString message;
    if (!validRegisterHeader(header, &error, &message)) return {};
    return encodeHeader(header);
}

MediaResult decodeDatagram(QByteArrayView payload, DecodedDatagram &datagram)
{
    MediaResult result;
    if (payload.size() < MediaHeaderBytes) {
        result.error = MediaError::TooShort;
        result.message = QStringLiteral("媒体负载短于 88 字节固定头");
        return result;
    }
    if (payload.size() > MaxMediaPayloadBytes) {
        result.error = MediaError::TooLong;
        result.message = QStringLiteral("媒体负载超过单条 1232 字节 UDP 的上限");
        return result;
    }
    const auto *data = reinterpret_cast<const uchar *>(payload.constData());
    if (std::memcmp(data, MediaMagic, 4) != 0) {
        result.error = MediaError::BadMagic;
        result.message = QStringLiteral("媒体 magic 不是 ZDS1");
        return result;
    }
    if (data[4] != MediaVersion) {
        result.error = MediaError::BadVersion;
        result.message = QStringLiteral("媒体协议版本不是 1");
        return result;
    }
    const quint8 rawType = data[5];
    if (rawType < 1 || rawType > 3) {
        result.error = MediaError::BadType;
        result.message = QStringLiteral("未知媒体包类型");
        return result;
    }
    MediaHeader header;
    header.type = MediaType(rawType);
    header.flags = get16(data + 6);
    std::memcpy(header.token.data(), data + 8, 16);
    header.sessionId = get64(data + 24);
    header.roomId = get64(data + 32);
    header.streamEpoch = get64(data + 40);
    header.sequence = get64(data + 48);
    header.pts = qint64(get64(data + 56));
    header.dts = qint64(get64(data + 64));
    header.duration = get32(data + 72);
    header.packetBytes = get32(data + 76);
    header.fragmentIndex = get16(data + 80);
    header.fragmentCount = get16(data + 82);
    header.fragmentBytes = get16(data + 84);
    if (get16(data + 86) != 0) {
        result.error = MediaError::BadReserved;
        result.message = QStringLiteral("保留字段必须为零");
        return result;
    }
    if ((header.flags & ~KnownFlags) != 0) {
        result.error = MediaError::BadFlags;
        result.message = QStringLiteral("flags 含未知位");
        return result;
    }
    if (header.type == MediaType::Register || header.type == MediaType::RegisterAck) {
        if (payload.size() != MediaHeaderBytes) {
            result.error = MediaError::BadLength;
            result.message = QStringLiteral("注册包不能携带负载");
            return result;
        }
        MediaError error = MediaError::None;
        QString message;
        if (!validRegisterHeader(header, &error, &message)) {
            result.error = error;
            result.message = message;
            return result;
        }
    } else {
        MediaError error = MediaError::None;
        QString message;
        if (!validVideoHeader(header, &error, &message)) {
            result.error = error;
            result.message = message;
            return result;
        }
        const int actualBytes = int(payload.size()) - MediaHeaderBytes;
        if (actualBytes != header.fragmentBytes) {
            result.error = MediaError::BadLength;
            result.message = QStringLiteral("实际片段长度与 fragmentBytes 不一致");
            return result;
        }
        datagram.fragment = QByteArray(payload.data() + MediaHeaderBytes, actualBytes);
    }
    datagram.header = header;
    result.ok = true;
    return result;
}

QByteArray encodeVideo(MediaHeader header, QByteArrayView fragment, MediaError *error,
                       QString *message)
{
    if (error) *error = MediaError::None;
    if (message) message->clear();
    if (fragment.size() > MaxFragmentBytes) {
        setError(error, message, MediaError::TooLong, QStringLiteral("单片超过 1125 字节"));
        return {};
    }
    header.type = MediaType::Video;
    header.fragmentBytes = quint16(fragment.size());
    MediaError headerError = MediaError::None;
    QString headerMessage;
    if (!validVideoHeader(header, &headerError, &headerMessage)) {
        setError(error, message, headerError, headerMessage);
        return {};
    }
    if (header.fragmentBytes != fragment.size()) {
        setError(error, message, MediaError::BadFragmentBytes,
                 QStringLiteral("fragmentBytes 与实际片段长度不一致"));
        return {};
    }
    QByteArray output = encodeHeader(header);
    output.append(fragment.data(), fragment.size());
    if (output.size() > MaxMediaPayloadBytes) {
        setError(error, message, MediaError::TooLong, QStringLiteral("媒体负载超过 1213 字节"));
        return {};
    }
    return output;
}

QList<QByteArray> fragmentVideo(const VideoPacket &packet, MediaError *error, QString *message)
{
    if (error) *error = MediaError::None;
    if (message) message->clear();
    MediaHeader header;
    header.type = MediaType::Video;
    header.flags = packet.flags;
    header.token = packet.token;
    header.sessionId = packet.sessionId;
    header.roomId = packet.roomId;
    header.streamEpoch = packet.streamEpoch;
    header.sequence = packet.sequence;
    header.pts = packet.pts;
    header.dts = packet.dts;
    header.duration = packet.duration;
    header.packetBytes = quint32(packet.data.size());
    if (packet.data.isEmpty()) {
        setError(error, message, MediaError::BadPacketBytes, QStringLiteral("编码包不能为空"));
        return {};
    }
    if (packet.data.size() > MaxVideoPacketBytes) {
        setError(error, message, MediaError::BadPacketBytes,
                 QStringLiteral("编码包超过 1 MiB 上限"));
        return {};
    }
    const quint32 count = expectedFragmentCount(header.packetBytes);
    if (count == 0 || count > MaxFragmentsPerPacket) {
        setError(error, message, MediaError::BadFragmentCount,
                 QStringLiteral("片数超过 1024 上限"));
        return {};
    }
    header.fragmentCount = quint16(count);
    header.fragmentIndex = 0;
    header.fragmentBytes = quint16(expectedFragmentBytes(header.packetBytes, 0));
    // Validate shared metadata once with the representative first fragment.
    if (!validVideoHeader(header, error, message)) return {};
    QList<QByteArray> fragments;
    fragments.reserve(count);
    for (quint16 index = 0; index < header.fragmentCount; ++index) {
        const quint32 length = expectedFragmentBytes(header.packetBytes, index);
        header.fragmentIndex = index;
        MediaHeader fragmentHeader = header;
        fragmentHeader.fragmentBytes = quint16(length);
        QByteArray fragment = packet.data.mid(int(index) * MaxFragmentBytes, int(length));
        MediaError fragmentError = MediaError::None;
        QString fragmentMessage;
        const QByteArray encoded = encodeVideo(fragmentHeader, fragment, &fragmentError,
                                               &fragmentMessage);
        if (encoded.isEmpty()) {
            setError(error, message, fragmentError, fragmentMessage);
            return {};
        }
        fragments.append(encoded);
    }
    return fragments;
}

bool fitsDatagram(int payloadBytes)
{
    return payloadBytes > 0 && payloadBytes <= MaxMediaPayloadBytes;
}

int datagramBytes(int payloadBytes) { return Zts1OuterBytes + payloadBytes; }

QByteArray wrapZts1(QByteArrayView payload)
{
    if (!fitsDatagram(int(payload.size()))) return {};
    const QByteArray encoded = protocol::encode(QByteArray(Route), QByteArray(payload.data(), int(payload.size())));
    if (encoded.isEmpty() || encoded.size() != datagramBytes(int(payload.size()))) return {};
    return encoded;
}

MediaResult unwrapZts1(QByteArrayView datagram, QByteArray &payload)
{
    MediaResult result;
    protocol::Packet packet;
    if (!protocol::decode(QByteArray(datagram.data(), int(datagram.size())), packet)) {
        result.error = MediaError::BadMagic;
        result.message = QStringLiteral("ZTS1 外层帧无效");
        return result;
    }
    if (packet.plugin != QByteArray(Route)) {
        result.error = MediaError::BadRoute;
        result.message = QStringLiteral("ZTS1 路由不是 desktop-share");
        return result;
    }
    if (packet.payload.size() > MaxMediaPayloadBytes) {
        result.error = MediaError::TooLong;
        result.message = QStringLiteral("ZTS1 负载超过 1232 字节 UDP 上限");
        return result;
    }
    payload = std::move(packet.payload);
    result.ok = true;
    return result;
}

bool sameFragmentMetadata(const MediaHeader &left, const MediaHeader &right)
{
    return left.type == right.type && left.flags == right.flags && left.token == right.token
        && left.sessionId == right.sessionId && left.roomId == right.roomId
        && left.streamEpoch == right.streamEpoch && left.sequence == right.sequence
        && left.pts == right.pts && left.dts == right.dts && left.duration == right.duration
        && left.packetBytes == right.packetBytes;
}

} // namespace vusiif::zto::desktop_share
