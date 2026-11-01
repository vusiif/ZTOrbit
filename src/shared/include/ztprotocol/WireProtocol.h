#pragma once
#include <QByteArray>
#include <QtEndian>

namespace vusiif::zto::protocol {
inline constexpr quint8 Version = 1;
inline constexpr int MaxTcpBytes = 1024 * 1024;
inline constexpr int MaxUdpBytes = 1232;
struct Packet { QByteArray plugin; QByteArray payload; };
inline bool validId(const QByteArray &id) {
    if (id.isEmpty() || id.size() > 128) return false;
    for (int i = 0; i < id.size(); ++i) {
        const auto c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}
inline QByteArray encode(const QByteArray &plugin, const QByteArray &payload) {
    if (!validId(plugin) || payload.size() > MaxTcpBytes - 6 - plugin.size()) return {};
    QByteArray bytes("ZTS1", 4);
    bytes.append(char(Version)); bytes.append(char(plugin.size()));
    return bytes + plugin + payload;
}
inline bool decode(const QByteArray &bytes, Packet &packet) {
    if (bytes.size() < 7 || bytes.size() > MaxTcpBytes || bytes.left(4) != "ZTS1" || quint8(bytes[4]) != Version) return false;
    const int length = quint8(bytes[5]);
    if (length < 1 || length > 128 || bytes.size() < 6 + length) return false;
    const auto id = bytes.mid(6, length);
    if (!validId(id)) return false;
    packet = {id, bytes.mid(6 + length)}; return true;
}
inline QByteArray frame(const QByteArray &plugin, const QByteArray &payload) {
    const auto packet = encode(plugin, payload);
    if (packet.isEmpty()) return {};
    QByteArray length(4, '\0');
    qToBigEndian(quint32(packet.size()), reinterpret_cast<uchar *>(length.data()));
    return length + packet;
}
enum class ReadResult { Incomplete, Packet, Invalid };
inline ReadResult takeFrame(QByteArray &input, Packet &packet) {
    if (input.size() < 4) return ReadResult::Incomplete;
    const auto length = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(input.constData()));
    if (length < 7 || length > MaxTcpBytes) return ReadResult::Invalid;
    if (input.size() < 4 + qsizetype(length)) return ReadResult::Incomplete;
    if (!decode(input.mid(4, length), packet)) return ReadResult::Invalid;
    input.remove(0, 4 + length); return ReadResult::Packet;
}
}
