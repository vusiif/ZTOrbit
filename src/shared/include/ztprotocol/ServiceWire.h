#pragma once
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
namespace vusiif::zto::protocol::service {
inline constexpr auto Protocol = "zto-service/1";
inline constexpr qsizetype MaxEnvelope = 64 * 1024, MaxPayload = 256 * 1024;
inline constexpr qsizetype MaxFrame = MaxEnvelope + MaxPayload + 8;
struct Message { QJsonObject header; QByteArray payload; };
inline QByteArray frame(const QJsonObject &header, const QByteArray &payload = {}) {
    const auto json=QJsonDocument(header).toJson(QJsonDocument::Compact);
    if(json.size()>MaxEnvelope || payload.size()>MaxPayload) return {};
    QByteArray out(8,Qt::Uninitialized);
    qToBigEndian(quint32(json.size()),reinterpret_cast<uchar *>(out.data()));
    qToBigEndian(quint32(payload.size()),reinterpret_cast<uchar *>(out.data()+4));
    return out+json+payload;
}
// 0 incomplete, -1 malformed, 1 complete. Callers cap socket read buffers too.
inline int take(QByteArray &buffer, Message &message) {
    if(buffer.size()<8) return 0;
    const auto json=qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(buffer.constData()));
    const auto data=qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(buffer.constData()+4));
    if(!json || json>MaxEnvelope || data>MaxPayload) return -1;
    const qsizetype size=8+qsizetype(json)+data;
    if(buffer.size()<size) return 0;
    QJsonParseError error; const auto doc=QJsonDocument::fromJson(buffer.mid(8,json),&error);
    if(error.error!=QJsonParseError::NoError || !doc.isObject()) return -1;
    message={doc.object(),buffer.mid(8+json,data)}; buffer.remove(0,size); return 1;
}
}
