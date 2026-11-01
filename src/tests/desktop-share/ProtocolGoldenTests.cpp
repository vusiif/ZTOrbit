#include <desktop-share/ControlProtocol.h>
#include <desktop-share/MediaProtocol.h>
#include <desktop-share/PendingRequests.h>
#include <desktop-share/SessionGate.h>
#include <ztprotocol/WireProtocol.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QtTest>

#ifndef ZDS_SAMPLES_DIR
#error "ZDS_SAMPLES_DIR must point at the shared desktop-share golden samples"
#endif

using namespace vusiif::zto::desktop_share;

namespace {

const QString kSamplesDir = QString::fromUtf8(ZDS_SAMPLES_DIR);

QJsonObject loadObject(const QString &name)
{
    QFile file(QDir(kSamplesDir).filePath(name));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QByteArray sampleBytes(const QJsonObject &sample)
{
    if (sample.contains(QStringLiteral("bytes")))
        return sample.value(QStringLiteral("bytes")).toString().toUtf8();
    return QByteArray::fromHex(sample.value(QStringLiteral("bytesHex")).toString().toLatin1());
}

QStringList stringArray(const QJsonArray &array)
{
    QStringList result;
    for (const auto &value : array) result.append(value.toString());
    return result;
}

void checkFormat(const QJsonObject &expect, const StreamFormat &format)
{
    if (expect.contains(QStringLiteral("formatCodec")))
        QCOMPARE(format.codec,expect.value(QStringLiteral("formatCodec")).toString());
    if (expect.contains(QStringLiteral("formatWidth")))
        QCOMPARE(format.width, expect.value(QStringLiteral("formatWidth")).toInt());
    if (expect.contains(QStringLiteral("formatHeight")))
        QCOMPARE(format.height, expect.value(QStringLiteral("formatHeight")).toInt());
    if (expect.contains(QStringLiteral("formatFps")))
        QCOMPARE(format.fps, expect.value(QStringLiteral("formatFps")).toInt());
    if (expect.contains(QStringLiteral("formatBitrate")))
        QCOMPARE(format.bitrate, expect.value(QStringLiteral("formatBitrate")).toInt());
    if (expect.contains(QStringLiteral("formatInit")))
        QCOMPARE(QString::fromLatin1(initializationDataToHex(format.initializationData)),
                 expect.value(QStringLiteral("formatInit")).toString());
    if (expect.contains(QStringLiteral("formatEpoch")))
        QCOMPARE(QString::number(format.streamEpoch),
                 expect.value(QStringLiteral("formatEpoch")).toString());
}

void checkRequest(const QJsonObject &expect, const Request &request)
{
    if (expect.contains(QStringLiteral("id")))
        QCOMPARE(qint64(request.id), expect.value(QStringLiteral("id")).toInteger());
    if (expect.contains(QStringLiteral("op")))
        QCOMPARE(operationName(request.op), expect.value(QStringLiteral("op")).toString());
    if (expect.contains(QStringLiteral("profile")))
        QCOMPARE(request.profile, expect.value(QStringLiteral("profile")).toString());
    if (expect.contains(QStringLiteral("roomCode")))
        QCOMPARE(request.roomCode, expect.value(QStringLiteral("roomCode")).toString());
    if (expect.contains(QStringLiteral("roomId")))
        QCOMPARE(QString::number(request.roomId),
                 expect.value(QStringLiteral("roomId")).toString());
    if (expect.contains(QStringLiteral("streamEpoch")))
        QCOMPARE(QString::number(request.streamEpoch),
                 expect.value(QStringLiteral("streamEpoch")).toString());
    if (expect.contains(QStringLiteral("completePackets")))
        QCOMPARE(QString::number(request.feedback.completePackets),
                 expect.value(QStringLiteral("completePackets")).toString());
    if (expect.contains(QStringLiteral("missingFragments")))
        QCOMPARE(QString::number(request.feedback.missingFragments),
                 expect.value(QStringLiteral("missingFragments")).toString());
    if (expect.contains(QStringLiteral("decodedFrames")))
        QCOMPARE(QString::number(request.feedback.decodedFrames),
                 expect.value(QStringLiteral("decodedFrames")).toString());
    if (expect.contains(QStringLiteral("displayedFrames")))
        QCOMPARE(QString::number(request.feedback.displayedFrames),
                 expect.value(QStringLiteral("displayedFrames")).toString());
    if (expect.contains(QStringLiteral("waiting")))
        QCOMPARE(request.feedback.waiting, expect.value(QStringLiteral("waiting")).toBool());
    if (expect.contains(QStringLiteral("receivedBytes")))
        QCOMPARE(QString::number(request.feedback.receivedBytes),
                 expect.value(QStringLiteral("receivedBytes")).toString());
    checkFormat(expect, request.format);
}

void checkRoom(const QJsonObject &expect, const RoomInfo &room)
{
    if (expect.contains(QStringLiteral("roomId")))
        QCOMPARE(QString::number(room.roomId), expect.value(QStringLiteral("roomId")).toString());
    if (expect.contains(QStringLiteral("roomCode")))
        QCOMPARE(room.roomCode, expect.value(QStringLiteral("roomCode")).toString());
    if (expect.contains(QStringLiteral("profile")))
        QCOMPARE(room.profile, expect.value(QStringLiteral("profile")).toString());
    if (expect.contains(QStringLiteral("role")))
        QCOMPARE(roleName(room.role), expect.value(QStringLiteral("role")).toString());
    if (expect.contains(QStringLiteral("token")))
        QCOMPARE(QString::fromLatin1(tokenToHex(room.token)),
                 expect.value(QStringLiteral("token")).toString());
    if (expect.contains(QStringLiteral("formatWidth"))) {
        QVERIFY(room.hasFormat);
        checkFormat(expect, room.format);
    }
}

void checkResponse(const QJsonObject &expect, const Response &response)
{
    if (expect.contains(QStringLiteral("id")))
        QCOMPARE(qint64(response.id), expect.value(QStringLiteral("id")).toInteger());
    if (expect.contains(QStringLiteral("ok")))
        QCOMPARE(response.ok, expect.value(QStringLiteral("ok")).toBool());
    if (!response.ok) {
        if (expect.contains(QStringLiteral("errorCode")))
            QCOMPARE(errorCodeName(response.error),
                     expect.value(QStringLiteral("errorCode")).toString());
        if (expect.contains(QStringLiteral("errorMessage")))
            QCOMPARE(response.message, expect.value(QStringLiteral("errorMessage")).toString());
        return;
    }
    switch (response.op) {
    case Operation::Hello:
        if (expect.contains(QStringLiteral("sessionId")))
            QCOMPARE(QString::number(response.hello.sessionId),
                     expect.value(QStringLiteral("sessionId")).toString());
        if (expect.contains(QStringLiteral("udpPort")))
            QCOMPARE(int(response.hello.udpPort),
                     expect.value(QStringLiteral("udpPort")).toInt());
        if (expect.contains(QStringLiteral("profiles")))
            QCOMPARE(response.hello.profiles,
                     stringArray(expect.value(QStringLiteral("profiles")).toArray()));
        if (expect.contains(QStringLiteral("maxViewers")))
            QCOMPARE(response.hello.maxViewers,
                     expect.value(QStringLiteral("maxViewers")).toInt());
        break;
    case Operation::Create:
    case Operation::Join:
        checkRoom(expect, response.room);
        break;
    default:
        if (expect.contains(QStringLiteral("roomId")))
            QCOMPARE(QString::number(response.roomId),
                     expect.value(QStringLiteral("roomId")).toString());
        if (expect.contains(QStringLiteral("streamEpoch")))
            QCOMPARE(QString::number(response.streamEpoch),
                     expect.value(QStringLiteral("streamEpoch")).toString());
        break;
    }
}

void checkEvent(const QJsonObject &expect, const Event &event)
{
    if (expect.contains(QStringLiteral("event")))
        QCOMPARE(eventName(event.kind), expect.value(QStringLiteral("event")).toString());
    if (expect.contains(QStringLiteral("roomId")))
        QCOMPARE(QString::number(event.roomId), expect.value(QStringLiteral("roomId")).toString());
    if (expect.contains(QStringLiteral("state")))
        QCOMPARE(roomStateName(event.roomState),
                 expect.value(QStringLiteral("state")).toString());
    if (expect.contains(QStringLiteral("viewerCount")))
        QCOMPARE(event.viewerCount, expect.value(QStringLiteral("viewerCount")).toInt());
    if (expect.contains(QStringLiteral("reason")))
        QCOMPARE(closeReasonName(event.reason),
                 expect.value(QStringLiteral("reason")).toString());
    if (expect.contains(QStringLiteral("streamEpoch")))
        QCOMPARE(QString::number(event.streamEpoch),
                 expect.value(QStringLiteral("streamEpoch")).toString());
    checkFormat(expect, event.format);
}

void checkMediaHeader(const QJsonObject &expect, const MediaHeader &header)
{
    if (expect.contains(QStringLiteral("type")))
        QCOMPARE(int(header.type), expect.value(QStringLiteral("type")).toInt());
    if (expect.contains(QStringLiteral("flags")))
        QCOMPARE(int(header.flags), expect.value(QStringLiteral("flags")).toInt());
    if (expect.contains(QStringLiteral("token")))
        QCOMPARE(QString::fromLatin1(tokenToHex(header.token)),
                 expect.value(QStringLiteral("token")).toString());
    if (expect.contains(QStringLiteral("sessionId")))
        QCOMPARE(QString::number(header.sessionId),
                 expect.value(QStringLiteral("sessionId")).toString());
    if (expect.contains(QStringLiteral("roomId")))
        QCOMPARE(QString::number(header.roomId),
                 expect.value(QStringLiteral("roomId")).toString());
    if (expect.contains(QStringLiteral("streamEpoch")))
        QCOMPARE(QString::number(header.streamEpoch),
                 expect.value(QStringLiteral("streamEpoch")).toString());
    if (expect.contains(QStringLiteral("sequence")))
        QCOMPARE(QString::number(header.sequence),
                 expect.value(QStringLiteral("sequence")).toString());
    if (expect.contains(QStringLiteral("pts")))
        QCOMPARE(QString::number(header.pts), expect.value(QStringLiteral("pts")).toString());
    if (expect.contains(QStringLiteral("dts")))
        QCOMPARE(QString::number(header.dts), expect.value(QStringLiteral("dts")).toString());
    if (expect.contains(QStringLiteral("duration")))
        QCOMPARE(qint64(header.duration), expect.value(QStringLiteral("duration")).toInteger());
    if (expect.contains(QStringLiteral("packetBytes")))
        QCOMPARE(QString::number(header.packetBytes),
                 expect.value(QStringLiteral("packetBytes")).toString());
    if (expect.contains(QStringLiteral("fragmentIndex")))
        QCOMPARE(int(header.fragmentIndex),
                 expect.value(QStringLiteral("fragmentIndex")).toInt());
    if (expect.contains(QStringLiteral("fragmentCount")))
        QCOMPARE(int(header.fragmentCount),
                 expect.value(QStringLiteral("fragmentCount")).toInt());
    if (expect.contains(QStringLiteral("fragmentBytes")))
        QCOMPARE(int(header.fragmentBytes),
                 expect.value(QStringLiteral("fragmentBytes")).toInt());
}

} // namespace

class ProtocolGoldenTests : public QObject {
    Q_OBJECT

private slots:
    void controlGolden();
    void controlInvalidGolden();
    void mediaGolden();
    void mediaInvalidGolden();
    void controlSizeBoundary();
    void deepNestingRejected();
    void udpFragmentBoundaries();
    void wrongRouteRejected();
    void zts1HalfAndStickyFrames();
    void pendingRequestTimeout();
    void sessionGate();
};

void ProtocolGoldenTests::controlGolden()
{
    const auto root = loadObject(QStringLiteral("control-golden.json"));
    QCOMPARE(root.value(QStringLiteral("format")).toString(),
             QStringLiteral("zds-control-golden/1"));
    const auto samples = root.value(QStringLiteral("valid")).toArray();
    QVERIFY(!samples.isEmpty());
    for (const auto &value : samples) {
        const auto sample = value.toObject();
        const auto name = sample.value(QStringLiteral("name")).toString();
        const auto bytes = sampleBytes(sample);
        const auto expect = sample.value(QStringLiteral("expect")).toObject();
        const auto kind = sample.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("request")) {
            Request request;
            const auto decoded = decodeRequest(bytes, request);
            QVERIFY2(decoded.ok, qPrintable(name + ": " + decoded.message));
            QCOMPARE(encodeRequest(request), bytes);
            checkRequest(expect, request);
        } else if (kind == QStringLiteral("response")) {
            const auto op = parseOperation(sample.value(QStringLiteral("op")).toString());
            QVERIFY(op.has_value());
            Response response;
            const auto decoded = decodeResponse(bytes, *op, response);
            QVERIFY2(decoded.ok, qPrintable(name + ": " + decoded.message));
            QCOMPARE(encodeResponse(response), bytes);
            checkResponse(expect, response);
        } else {
            Event event;
            const auto decoded = decodeEvent(bytes, event);
            QVERIFY2(decoded.ok, qPrintable(name + ": " + decoded.message));
            QCOMPARE(encodeEvent(event), bytes);
            checkEvent(expect, event);
        }
    }
}

void ProtocolGoldenTests::controlInvalidGolden()
{
    const auto root = loadObject(QStringLiteral("control-golden.json"));
    for (const auto &value : root.value(QStringLiteral("invalid")).toArray()) {
        const auto sample = value.toObject();
        const auto name = sample.value(QStringLiteral("name")).toString();
        const auto bytes = sampleBytes(sample);
        const auto expected = sample.value(QStringLiteral("error")).toString();
        const auto kind = sample.value(QStringLiteral("kind")).toString();
        DecodeResult result;
        if (kind == QStringLiteral("request")) {
            Request request;
            result = decodeRequest(bytes, request);
        } else if (kind == QStringLiteral("response")) {
            const auto op = parseOperation(sample.value(QStringLiteral("op")).toString());
            QVERIFY(op.has_value());
            Response response;
            result = decodeResponse(bytes, *op, response);
        } else {
            Event event;
            result = decodeEvent(bytes, event);
        }
        QVERIFY2(!result.ok, qPrintable(name + ": 非法样例被接受"));
        QCOMPARE(errorCodeName(result.error), expected);
    }
}

void ProtocolGoldenTests::mediaGolden()
{
    const auto root = loadObject(QStringLiteral("media-golden.json"));
    QCOMPARE(root.value(QStringLiteral("format")).toString(),
             QStringLiteral("zds-media-golden/1"));
    for (const auto &value : root.value(QStringLiteral("valid")).toArray()) {
        const auto sample = value.toObject();
        const auto name = sample.value(QStringLiteral("name")).toString();
        const auto bytes = sampleBytes(sample);
        const auto expect = sample.value(QStringLiteral("expect")).toObject();
        DecodedDatagram datagram;
        const auto decoded = decodeDatagram(bytes, datagram);
        QVERIFY2(decoded.ok, qPrintable(name + ": " + decoded.message));
        checkMediaHeader(expect, datagram.header);
        const auto expectedDatagram = sample.value(QStringLiteral("zts1DatagramBytes")).toInt();
        QCOMPARE(datagramBytes(bytes.size()), expectedDatagram);
        const auto wrapped = wrapZts1(bytes);
        QCOMPARE(wrapped.size(), expectedDatagram);
        QByteArray unwrapped;
        const auto unwrapResult = unwrapZts1(wrapped, unwrapped);
        QVERIFY2(unwrapResult.ok, qPrintable(name + ": " + unwrapResult.message));
        QCOMPARE(unwrapped, bytes);
        if (datagram.header.type == MediaType::Register) {
            QCOMPARE(encodeRegister(datagram.header.token, datagram.header.sessionId,
                                    datagram.header.roomId),
                     bytes);
        } else if (datagram.header.type == MediaType::RegisterAck) {
            QCOMPARE(encodeRegisterAck(datagram.header.token, datagram.header.sessionId,
                                       datagram.header.roomId),
                     bytes);
        } else {
            QCOMPARE(encodeVideo(datagram.header, datagram.fragment), bytes);
        }
    }
}

void ProtocolGoldenTests::mediaInvalidGolden()
{
    const auto root = loadObject(QStringLiteral("media-golden.json"));
    for (const auto &value : root.value(QStringLiteral("invalid")).toArray()) {
        const auto sample = value.toObject();
        const auto name = sample.value(QStringLiteral("name")).toString();
        const auto bytes = sampleBytes(sample);
        const auto expected = sample.value(QStringLiteral("error")).toString();
        DecodedDatagram datagram;
        const auto decoded = decodeDatagram(bytes, datagram);
        QVERIFY2(!decoded.ok, qPrintable(name + ": 非法样例被接受"));
        QCOMPARE(mediaErrorName(decoded.error), expected);
    }
}

void ProtocolGoldenTests::controlSizeBoundary()
{
    const QByteArray prefix = R"({"v":1,"id":1,"op":"hello","data":{},"x":")";
    const QByteArray suffix = R"("})";
    QByteArray exact = prefix + QByteArray(MaxControlBytes - prefix.size() - suffix.size(), 'a')
        + suffix;
    QCOMPARE(exact.size(), int(MaxControlBytes));
    Request request;
    QVERIFY(decodeRequest(exact, request).ok);
    QByteArray tooLarge = exact;
    tooLarge.insert(tooLarge.size() - 1, 'a');
    QCOMPARE(tooLarge.size(), int(MaxControlBytes) + 1);
    const auto decoded = decodeRequest(tooLarge, request);
    QVERIFY(!decoded.ok);
    QCOMPARE(decoded.error, ErrorCode::InvalidRequest);
}

void ProtocolGoldenTests::deepNestingRejected()
{
    QByteArray bytes = R"({"v":1,"id":1,"op":"hello","data":{"x":)";
    bytes += QByteArray(70, '[');
    bytes += QByteArray(70, ']');
    bytes += "}}";
    Request request;
    const auto decoded = decodeRequest(bytes, request);
    QVERIFY(!decoded.ok);
    QCOMPARE(decoded.error, ErrorCode::InvalidRequest);
}

void ProtocolGoldenTests::udpFragmentBoundaries()
{
    VideoPacket packet;
    packet.token = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    packet.sessionId = 1;
    packet.roomId = 7;
    packet.streamEpoch = 1;
    packet.sequence = 0;
    packet.flags = quint16(MediaFlag::KeyFrame);
    packet.pts = 1000;
    packet.duration = 33333;
    packet.data = QByteArray(MaxVideoPacketBytes, 'x');
    MediaError error = MediaError::None;
    QString message;
    const auto fragments = fragmentVideo(packet, &error, &message);
    QVERIFY2(error == MediaError::None, qPrintable(message));
    QCOMPARE(fragments.size(), 933);
    MediaHeader firstHeader;
    QByteArray firstPayload;
    {
        DecodedDatagram decoded;
        const auto result = decodeDatagram(fragments.first(), decoded);
        QVERIFY2(result.ok, qPrintable(result.message));
        firstHeader = decoded.header;
        firstPayload = fragments.first();
    }
    for (int i = 0; i < fragments.size(); ++i) {
        QVERIFY(fragments[i].size() <= MaxMediaPayloadBytes);
        QCOMPARE(datagramBytes(fragments[i].size()),
                 i == fragments.size() - 1 ? 19 + 88 + 76 : 1232);
        DecodedDatagram decoded;
        const auto result = decodeDatagram(fragments[i], decoded);
        QVERIFY2(result.ok, qPrintable(result.message));
        QCOMPARE(int(decoded.header.fragmentIndex), i);
        QCOMPARE(int(decoded.header.fragmentCount), 933);
        QVERIFY(sameFragmentMetadata(firstHeader, decoded.header));
    }
    // A complete 1232-byte datagram is accepted; one extra byte is not.
    QCOMPARE(firstPayload.size(), MaxMediaPayloadBytes);
    QVERIFY(wrapZts1(QByteArray(MaxMediaPayloadBytes + 1, 'x')).isEmpty());
    DecodedDatagram oversized;
    const auto oversizedResult = decodeDatagram(QByteArray(MaxMediaPayloadBytes + 1, 'x'),
                                                oversized);
    QVERIFY(!oversizedResult.ok);
    QCOMPARE(oversizedResult.error, MediaError::TooLong);
}

void ProtocolGoldenTests::wrongRouteRejected()
{
    VideoPacket packet;
    packet.token = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    packet.sessionId = 1;
    packet.roomId = 7;
    packet.streamEpoch = 1;
    packet.flags = quint16(MediaFlag::KeyFrame);
    packet.data = QByteArray(1, 'x');
    const auto fragments = fragmentVideo(packet);
    QCOMPARE(fragments.size(), 1);
    const auto wrong = vusiif::zto::protocol::encode("other-plugin", fragments.first());
    QVERIFY(!wrong.isEmpty());
    QByteArray payload;
    const auto result = unwrapZts1(wrong, payload);
    QVERIFY(!result.ok);
    QCOMPARE(result.error, MediaError::BadRoute);
}

void ProtocolGoldenTests::zts1HalfAndStickyFrames()
{
    using namespace vusiif::zto::protocol;
    const QByteArray one = "one";
    const QByteArray two = "two";
    const auto first = frame("desktop-share", one);
    const auto second = frame("desktop-share", two);
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());

    QByteArray input = first.left(3);
    Packet packet;
    QCOMPARE(takeFrame(input, packet), ReadResult::Incomplete);
    input = first.left(first.size() - 1);
    QCOMPARE(takeFrame(input, packet), ReadResult::Incomplete);
    input = first;
    QCOMPARE(takeFrame(input, packet), ReadResult::Packet);
    QCOMPARE(packet.plugin, QByteArray("desktop-share"));
    QCOMPARE(packet.payload, one);
    QVERIFY(input.isEmpty());

    input = first + second;
    QCOMPARE(takeFrame(input, packet), ReadResult::Packet);
    QCOMPARE(packet.payload, one);
    QCOMPARE(takeFrame(input, packet), ReadResult::Packet);
    QCOMPARE(packet.payload, two);
    QVERIFY(input.isEmpty());

    QByteArray invalid(4, '\0');
    qToBigEndian(quint32(6), reinterpret_cast<uchar *>(invalid.data()));
    QCOMPARE(takeFrame(invalid, packet), ReadResult::Invalid);
}

void ProtocolGoldenTests::sessionGate()
{
    const auto isStream = [](Operation operation) {
        return operation == Operation::StreamConfigure || operation == Operation::StreamReady
            || operation == Operation::StreamKeyframe || operation == Operation::StreamFeedback;
    };
    for (int i = 0; i <= int(Operation::StreamFeedback); ++i) {
        const auto operation = Operation(i);
        const auto name = operationName(operation);
        // Before hello only the handshake is legal.
        if (operation == Operation::Hello) {
            QVERIFY2(operationAllowed(SessionPhase::Unhandshaken, operation), qPrintable(name));
            QCOMPARE(operationGateError(SessionPhase::Unhandshaken, operation), ErrorCode::None);
        } else {
            QVERIFY2(!operationAllowed(SessionPhase::Unhandshaken, operation), qPrintable(name));
            QCOMPARE(operationGateError(SessionPhase::Unhandshaken, operation),
                     ErrorCode::NotReady);
        }
        // Idle: create/join and the idempotent leave/stop reach room logic; a
        // stream operation still requires a room.
        if (isStream(operation)) {
            QVERIFY2(!operationAllowed(SessionPhase::Idle, operation), qPrintable(name));
            QCOMPARE(operationGateError(SessionPhase::Idle, operation), ErrorCode::NotReady);
        } else {
            QVERIFY2(operationAllowed(SessionPhase::Idle, operation), qPrintable(name));
            QCOMPARE(operationGateError(SessionPhase::Idle, operation), ErrorCode::None);
        }
        // In a room the gate has no room or role context: every operation must
        // reach phase-2 room logic.
        QVERIFY2(operationAllowed(SessionPhase::InRoom, operation), qPrintable(name));
        QCOMPARE(operationGateError(SessionPhase::InRoom, operation), ErrorCode::None);
    }
    // Explicit coverage for the regression paths reported in STEP.md.
    QCOMPARE(operationGateError(SessionPhase::Idle, Operation::Leave), ErrorCode::None);
    QCOMPARE(operationGateError(SessionPhase::Idle, Operation::Stop), ErrorCode::None);
    QCOMPARE(operationGateError(SessionPhase::InRoom, Operation::Create), ErrorCode::None);
    QCOMPARE(operationGateError(SessionPhase::InRoom, Operation::Join), ErrorCode::None);
    QVERIFY(operationGateError(SessionPhase::InRoom, Operation::Join)
            != ErrorCode::AlreadyInRoom);
}

void ProtocolGoldenTests::pendingRequestTimeout()
{
    PendingRequests pending;
    for (quint32 id = 1; id <= quint32(PendingRequests::MaxOutstanding); ++id)
        QVERIFY(pending.add(id, 1000));
    QCOMPARE(pending.size(), PendingRequests::MaxOutstanding);
    QVERIFY(!pending.add(PendingRequests::MaxOutstanding + 1, 1000));
    QVERIFY(!pending.add(1, 1000));
    QVERIFY(pending.complete(1, 1000 + PendingRequests::TimeoutMs - 1));
    QVERIFY(!pending.complete(1, 1000 + PendingRequests::TimeoutMs));
    QVERIFY(!pending.complete(2, 1000 + PendingRequests::TimeoutMs));
    QCOMPARE(pending.expire(1000 + PendingRequests::TimeoutMs), 14);
    QCOMPARE(pending.size(), 0);
    // A late response never resurrects state after the deadline.
    QVERIFY(pending.add(99, 5000));
    QVERIFY(!pending.complete(99, 5000 + PendingRequests::TimeoutMs));
    QVERIFY(!pending.contains(99, 5000 + PendingRequests::TimeoutMs));
}

QTEST_GUILESS_MAIN(ProtocolGoldenTests)
#include "ProtocolGoldenTests.moc"
