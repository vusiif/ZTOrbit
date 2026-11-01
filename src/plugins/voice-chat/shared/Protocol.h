#pragma once
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

namespace vusiif::zto::voice_chat {
inline constexpr int ProtocolVersion = 1;
inline constexpr quint16 DefaultPort = 18100;
inline constexpr int MaxControlBytes = 65536;
inline constexpr int MaxDatagramBytes = 1200;
inline constexpr int HeaderBytes = 30;
inline constexpr int FrameSamples = 960;
enum PacketType : quint8 { Register = 1, Registered = 2, Audio = 3 };
struct Packet {
    quint8 type = 0;
    QByteArray token;
    quint32 sender = 0;
    quint32 sequence = 0;
    QByteArray payload;
};
inline QByteArray encodePacket(const Packet &p)
{
    if (p.token.size() != 16 || p.payload.size() > MaxDatagramBytes - HeaderBytes) return {};
    QByteArray data(HeaderBytes, '\0');
    data.replace(0, 4, QByteArrayLiteral("ZTV1"));
    data[4] = ProtocolVersion;
    data[5] = p.type;
    data.replace(6, 16, p.token);
    qToBigEndian(p.sender, reinterpret_cast<uchar *>(data.data() + 22));
    qToBigEndian(p.sequence, reinterpret_cast<uchar *>(data.data() + 26));
    return data + p.payload;
}
inline bool decodePacket(const QByteArray &data, Packet &p)
{
    if (data.size() < HeaderBytes || data.size() > MaxDatagramBytes || data.left(4) != "ZTV1"
        || quint8(data[4]) != ProtocolVersion) return false;
    p.type = quint8(data[5]);
    if (p.type < Register || p.type > Audio) return false;
    p.token = data.mid(6, 16);
    p.sender = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(data.constData() + 22));
    p.sequence = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(data.constData() + 26));
    p.payload = data.mid(HeaderBytes);
    return p.type == Audio ? !p.payload.isEmpty() : p.payload.isEmpty();
}
inline QByteArray jsonLine(const QJsonObject &value)
{
    return QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
}
}
