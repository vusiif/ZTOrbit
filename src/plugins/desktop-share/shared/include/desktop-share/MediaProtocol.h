#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>

#include <array>
#include <cstdint>
#include <optional>

namespace vusiif::zto::desktop_share {

inline constexpr char MediaMagic[] = "ZDS1";
inline constexpr int MediaVersion = 1;
inline constexpr int MediaHeaderBytes = 88;
inline constexpr int MaxVideoPacketBytes = 1024 * 1024;
inline constexpr int MaxFragmentsPerPacket = 1024;
inline constexpr int MaxFragmentBytes = 1125;
inline constexpr int MaxUdpDatagramBytes = 1232;
inline constexpr int Zts1OuterBytes = 6 + 13; // "ZTS1" + version + length + "desktop-share"
inline constexpr int MaxMediaPayloadBytes = MaxUdpDatagramBytes - Zts1OuterBytes; // 1213

enum class MediaType : quint8 {
    Register = 1,
    RegisterAck = 2,
    Video = 3,
};

enum class MediaFlag : quint16 {
    KeyFrame = 0x0001,
    Discontinuity = 0x0002,
    HasDts = 0x0004,
};

// Invalid input is rejected before memory is allocated or state is changed.
enum class MediaError : quint8 {
    None = 0,
    TooShort,
    TooLong,
    BadMagic,
    BadVersion,
    BadType,
    BadFlags,
    BadReserved,
    BadToken,
    BadSession,
    BadRoom,
    BadEpoch,
    BadPacketBytes,
    BadFragmentCount,
    BadFragmentIndex,
    BadFragmentBytes,
    BadLength,
    BadTimestamp,
    RegisterFields,
    BadRoute,
    Overflow,
};

QString mediaErrorName(MediaError error);

struct MediaHeader {
    MediaType type = MediaType::Video;
    quint16 flags = 0;
    std::array<quint8, 16> token{};
    quint64 sessionId = 0;
    quint64 roomId = 0;
    quint64 streamEpoch = 0;
    quint64 sequence = 0;
    qint64 pts = 0;
    qint64 dts = 0;
    quint32 duration = 0;
    quint32 packetBytes = 0;
    quint16 fragmentIndex = 0;
    quint16 fragmentCount = 0;
    quint16 fragmentBytes = 0;

    bool keyFrame() const { return (flags & quint16(MediaFlag::KeyFrame)) != 0; }
    bool discontinuity() const { return (flags & quint16(MediaFlag::Discontinuity)) != 0; }
    bool hasDts() const { return (flags & quint16(MediaFlag::HasDts)) != 0; }
};

struct VideoPacket {
    quint16 flags = 0;
    std::array<quint8, 16> token{};
    quint64 sessionId = 0;
    quint64 roomId = 0;
    quint64 streamEpoch = 0;
    quint64 sequence = 0;
    qint64 pts = 0;
    qint64 dts = 0;
    quint32 duration = 0;
    QByteArray data;

    bool keyFrame() const { return (flags & quint16(MediaFlag::KeyFrame)) != 0; }
    bool discontinuity() const { return (flags & quint16(MediaFlag::Discontinuity)) != 0; }
    bool hasDts() const { return (flags & quint16(MediaFlag::HasDts)) != 0; }
};

struct DecodedDatagram {
    MediaHeader header;
    QByteArray fragment;
};

struct MediaResult {
    bool ok = false;
    MediaError error = MediaError::None;
    QString message;

    explicit operator bool() const { return ok; }
};

// Fixed 88-byte register/ack payloads. Both carry the media token, the owning
// session and the internal room ID; every other header field is zero.
QByteArray encodeRegister(const std::array<quint8, 16> &token, quint64 sessionId, quint64 roomId);
QByteArray encodeRegisterAck(const std::array<quint8, 16> &token, quint64 sessionId, quint64 roomId);

// Decodes one complete business payload (88-byte header plus fragment).
MediaResult decodeDatagram(QByteArrayView payload, DecodedDatagram &datagram);

// Encodes one video fragment with an explicit header. The header's
// fragmentBytes must match the fragment size.
QByteArray encodeVideo(MediaHeader header, QByteArrayView fragment, MediaError *error = nullptr,
                       QString *message = nullptr);

// Splits one complete encoded access unit into 1125-byte fragments. Returns
// fragments in order; on failure the list is empty and the error is reported.
QList<QByteArray> fragmentVideo(const VideoPacket &packet, MediaError *error = nullptr,
                                QString *message = nullptr);

// True for payload sizes that fit a complete ZTS1 UDP datagram.
bool fitsDatagram(int payloadBytes);
int datagramBytes(int payloadBytes);

// ZTS1 wrapper using the installed public protocol header. wrapZts1 returns an
// empty array for payloads that do not fit or for an invalid route.
QByteArray wrapZts1(QByteArrayView payload);
MediaResult unwrapZts1(QByteArrayView datagram, QByteArray &payload);

// Metadata that must be identical across all fragments of one encoded packet.
bool sameFragmentMetadata(const MediaHeader &left, const MediaHeader &right);

} // namespace vusiif::zto::desktop_share
