#include <desktop-share/ControlProtocol.h>

#include "JsonSupport.h"

#include <QByteArrayView>
#include <QtGlobal>

#include <algorithm>
#include <limits>

namespace vusiif::zto::desktop_share {
namespace {

using json::Value;

constexpr quint32 MaxRequestId = 0xFFFF'FFFFu;

bool asciiDigit(QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); }
bool asciiHex(QChar c)
{
    return asciiDigit(c) || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
        || (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
}

const ProfileInfo Profiles[] = {
    {QStringView(u"720p30"), 1280, 720, 30, 2'000'000},
    {QStringView(u"1080p30"), 1920, 1080, 30, 4'000'000},
};

QByteArray toBytes(QByteArrayView view)
{
    if (view.isEmpty()) return {};
    return QByteArray(view.data(), int(view.size()));
}

void writeU64String(json::Writer &writer, QStringView key, quint64 value)
{
    writer.key(key);
    writer.string(QString::number(value));
}

void fail(DecodeResult &result, ErrorCode code, const QString &message)
{
    result.ok = false;
    result.error = code;
    result.message = message;
}

bool requireMember(const Value &object, QStringView key, const Value *&out, DecodeResult &result,
                   QStringView what)
{
    out = object.member(key);
    if (!out) {
        fail(result, ErrorCode::InvalidRequest,
             QStringLiteral("缺少字段 %1（%2）").arg(key.toString(), what.toString()));
        return false;
    }
    return true;
}

bool readString(const Value &object, QStringView key, QString &out, DecodeResult &result,
                bool required = true)
{
    const Value *value = object.member(key);
    if (!value) {
        if (!required) return true;
        fail(result, ErrorCode::InvalidRequest,
             QStringLiteral("缺少字符串字段 %1").arg(key.toString()));
        return false;
    }
    if (!value->isString()) {
        fail(result, ErrorCode::InvalidRequest,
             QStringLiteral("字段 %1 必须是字符串").arg(key.toString()));
        return false;
    }
    out = value->text();
    return true;
}

bool readBool(const Value &object, QStringView key, bool &out, DecodeResult &result)
{
    const Value *value = nullptr;
    if (!requireMember(object, key, value, result, QStringLiteral("布尔字段"))) return false;
    if (!value->isBool()) {
        fail(result, ErrorCode::InvalidRequest,
             QStringLiteral("字段 %1 必须是布尔值").arg(key.toString()));
        return false;
    }
    out = value->boolean();
    return true;
}

bool readUint(const Value &object, QStringView key, int minimum, int maximum, int &out,
              DecodeResult &result)
{
    const Value *value = nullptr;
    if (!requireMember(object, key, value, result, QStringLiteral("整数字段"))) return false;
    const auto integer = value->integer();
    if (!integer || *integer < minimum || *integer > maximum) {
        fail(result, ErrorCode::InvalidRequest,
             QStringLiteral("字段 %1 必须是 %2..%3 的整数").arg(key.toString()).arg(minimum).arg(maximum));
        return false;
    }
    out = int(*integer);
    return true;
}

bool readU64String(const Value &object, QStringView key, bool allowZero, quint64 &out,
                   DecodeResult &result)
{
    QString text;
    if (!readString(object, key, text, result)) return false;
    const auto value = parseU64(text);
    if (!value || (!allowZero && *value == 0)) {
        fail(result, ErrorCode::InvalidRequest,
             QStringLiteral("字段 %1 必须是无前导零的十进制无符号整数").arg(key.toString()));
        return false;
    }
    out = *value;
    return true;
}

bool readObject(const Value &object, QStringView key, const Value *&out, DecodeResult &result)
{
    if (!requireMember(object, key, out, result, QStringLiteral("对象字段"))) return false;
    if (!out->isObject()) {
        fail(result, ErrorCode::InvalidRequest,
             QStringLiteral("字段 %1 必须是对象").arg(key.toString()));
        return false;
    }
    return true;
}

bool readHexString(const Value &object, QStringView key, QByteArray &out, DecodeResult &result)
{
    QString text;
    if (!readString(object, key, text, result)) return false;
    const auto decoded = initializationDataFromHex(text);
    if (!decoded) {
        fail(result, ErrorCode::InvalidRequest,
             QStringLiteral("字段 %1 必须是偶数长度、不超过 %2 字节的十六进制字符串")
                 .arg(key.toString())
                 .arg(MaxInitializationBytes));
        return false;
    }
    out = *decoded;
    return true;
}

bool writeFormatDimensions(const StreamFormat &format)
{
    return profileInfoForDimensions(format.width, format.height, format.fps, format.bitrate) != nullptr
        && format.initializationData.size() <= MaxInitializationBytes
        && (format.codec == QStringLiteral("h264") ||
            (format.codec == QStringLiteral("bgra-tiles") && format.initializationData.isEmpty()));
}

void writeFormat(json::Writer &writer, const StreamFormat &format)
{
    writer.beginObject();
    writer.key(u"codec");
    writer.string(format.codec);
    writer.key(u"width");
    writer.number(format.width);
    writer.key(u"height");
    writer.number(format.height);
    writer.key(u"fps");
    writer.number(format.fps);
    writer.key(u"bitrate");
    writer.number(format.bitrate);
    writer.key(u"initializationData");
    writer.string(QString::fromLatin1(initializationDataToHex(format.initializationData)));
    writer.key(u"streamEpoch");
    writer.string(QString::number(format.streamEpoch));
    writer.endObject();
}

bool readFormat(const Value &object, StreamFormat &format, DecodeResult &result)
{
    QString codec;
    if (!readString(object, u"codec", codec, result)) return false;
    if (codec != QStringLiteral("h264") && codec != QStringLiteral("bgra-tiles")) {
        fail(result, ErrorCode::InvalidRequest, QStringLiteral("codec 只接受 h264 或 bgra-tiles"));
        return false;
    }
    format.codec = codec;
    if (!readUint(object, u"width", 2, 8192, format.width, result)) return false;
    if (!readUint(object, u"height", 2, 8192, format.height, result)) return false;
    if (!readUint(object, u"fps", 1, 120, format.fps, result)) return false;
    if (!readUint(object, u"bitrate", 1, 100'000'000, format.bitrate, result)) return false;
    if (!readHexString(object, u"initializationData", format.initializationData, result)) return false;
    if (codec == QStringLiteral("bgra-tiles") && !format.initializationData.isEmpty()) {
        fail(result,ErrorCode::InvalidRequest,QStringLiteral("bgra-tiles 初始化数据必须为空")); return false;
    }
    if (!readU64String(object, u"streamEpoch", false, format.streamEpoch, result)) return false;
    if (!profileInfoForDimensions(format.width, format.height, format.fps, format.bitrate)) {
        fail(result, ErrorCode::InvalidRequest,
             QStringLiteral("格式尺寸、帧率与码率必须匹配已定义档位"));
        return false;
    }
    return true;
}

bool writeHello(const HelloInfo &hello)
{
    if (hello.sessionId == 0 || hello.udpPort == 0 || hello.maxViewers < 1
        || hello.maxViewers > MaxViewersPerRoom || hello.profiles.isEmpty()
        || hello.profiles.size() > 2)
        return false;
    for (const auto &name : hello.profiles)
        if (!profileInfo(name)) return false;
    for (int i = 0; i < hello.profiles.size(); ++i)
        for (int j = i + 1; j < hello.profiles.size(); ++j)
            if (hello.profiles[i] == hello.profiles[j]) return false;
    return true;
}

void writeHelloData(json::Writer &writer, const HelloInfo &hello)
{
    writeU64String(writer, u"sessionId", hello.sessionId);
    writer.key(u"udpPort");
    writer.number(int(hello.udpPort));
    writer.key(u"capabilities");
    writer.beginObject();
    writer.key(u"profiles");
    writer.beginArray();
    // Normalize to the contract order so decoding and re-encoding is stable.
    for (const auto &profile : Profiles)
        if (hello.profiles.contains(QString(profile.name))) writer.string(profile.name);
    writer.endArray();
    writer.key(u"maxViewers");
    writer.number(hello.maxViewers);
    writer.endObject();
}

bool readHello(const Value &data, HelloInfo &hello, DecodeResult &result)
{
    if (!readU64String(data, u"sessionId", false, hello.sessionId, result)) return false;
    int port = 0;
    if (!readUint(data, u"udpPort", 1, 65'535, port, result)) return false;
    hello.udpPort = quint16(port);
    const Value *capabilities = nullptr;
    if (!readObject(data, u"capabilities", capabilities, result)) return false;
    const Value *profiles = nullptr;
    if (!requireMember(*capabilities, u"profiles", profiles, result, QStringLiteral("字符串数组")))
        return false;
    if (!profiles->isArray() || profiles->elements().empty() || profiles->elements().size() > 2) {
        fail(result, ErrorCode::InvalidRequest, QStringLiteral("capabilities.profiles 必须包含 1~2 项"));
        return false;
    }
    for (const auto &entry : profiles->elements()) {
        if (!entry.isString() || !profileInfo(entry.text())) {
            fail(result, ErrorCode::InvalidRequest, QStringLiteral("capabilities.profiles 含未知档位"));
            return false;
        }
        if (hello.profiles.contains(entry.text())) {
            fail(result, ErrorCode::InvalidRequest, QStringLiteral("capabilities.profiles 含重复档位"));
            return false;
        }
        hello.profiles.append(entry.text());
    }
    if (!readUint(*capabilities, u"maxViewers", 1, MaxViewersPerRoom, hello.maxViewers, result))
        return false;
    return true;
}

bool writeRoom(const RoomInfo &room)
{
    if (room.roomId == 0 || !isValidRoomCode(room.roomCode) || !profileInfo(room.profile)
        || room.token.size() != 16)
        return false;
    if (room.hasFormat) {
        if (room.role != Role::Viewer) return false;
        if (!writeFormatDimensions(room.format) || room.format.streamEpoch == 0) return false;
        if (room.profile != profileInfoForDimensions(room.format.width, room.format.height,
                                                     room.format.fps, room.format.bitrate)->name)
            return false;
    }
    return true;
}

void writeRoomData(json::Writer &writer, const RoomInfo &room)
{
    writeU64String(writer, u"roomId", room.roomId);
    writer.key(u"roomCode");
    writer.string(room.roomCode);
    writer.key(u"profile");
    writer.string(room.profile);
    writer.key(u"role");
    writer.string(roleName(room.role));
    writer.key(u"token");
    writer.string(QString::fromLatin1(tokenToHex(room.token)));
    if (room.hasFormat) {
        writer.key(u"format");
        writeFormat(writer, room.format);
    }
}

bool readRoom(const Value &data, Role expectedRole, RoomInfo &room, DecodeResult &result)
{
    if (!readU64String(data, u"roomId", false, room.roomId, result)) return false;
    if (!readString(data, u"roomCode", room.roomCode, result)) return false;
    if (!isValidRoomCode(room.roomCode)) {
        fail(result, ErrorCode::InvalidRequest, QStringLiteral("roomCode 必须是八位十进制字符串"));
        return false;
    }
    if (!readString(data, u"profile", room.profile, result)) return false;
    if (!profileInfo(room.profile)) {
        fail(result, ErrorCode::InvalidRequest, QStringLiteral("profile 不是已定义档位"));
        return false;
    }
    QString roleText;
    if (!readString(data, u"role", roleText, result)) return false;
    const auto role = parseRole(roleText);
    if (!role || *role != expectedRole) {
        fail(result, ErrorCode::InvalidRequest, QStringLiteral("响应角色与请求不匹配"));
        return false;
    }
    room.role = *role;
    QString tokenText;
    if (!readString(data, u"token", tokenText, result)) return false;
    const auto token = tokenFromHex(tokenText);
    if (!token) {
        fail(result, ErrorCode::InvalidRequest, QStringLiteral("token 必须是 32 字符十六进制字符串"));
        return false;
    }
    room.token = *token;
    if (const Value *format = data.member(u"format")) {
        if (expectedRole != Role::Viewer) {
            fail(result, ErrorCode::InvalidRequest, QStringLiteral("create 响应不能包含 format"));
            return false;
        }
        if (!format->isObject()) {
            fail(result, ErrorCode::InvalidRequest, QStringLiteral("format 必须是对象"));
            return false;
        }
        if (!readFormat(*format, room.format, result)) return false;
        const ProfileInfo *profile = profileInfo(room.profile);
        if (!profile || profile->width != room.format.width || profile->height != room.format.height
            || profile->fps != room.format.fps || profile->bitrate != room.format.bitrate) {
            fail(result, ErrorCode::InvalidRequest, QStringLiteral("format 与房间档位不一致"));
            return false;
        }
        room.hasFormat = true;
    }
    return true;
}

bool decodeEnvelope(QByteArrayView bytes, Value &root, DecodeResult &result)
{
    if (bytes.isEmpty()) {
        fail(result, ErrorCode::InvalidRequest, QStringLiteral("控制消息为空"));
        return false;
    }
    if (bytes.size() > MaxControlBytes) {
        fail(result, ErrorCode::InvalidRequest,
             QStringLiteral("控制消息超过 %1 字节上限").arg(MaxControlBytes));
        return false;
    }
    json::ParseError parseError;
    const auto parsed = json::parse(bytes, &parseError);
    if (!parsed) {
        fail(result, ErrorCode::InvalidRequest,
             QStringLiteral("JSON 解析失败（偏移 %1）：%2").arg(parseError.offset).arg(parseError.message));
        return false;
    }
    if (!parsed->isObject()) {
        fail(result, ErrorCode::InvalidRequest, QStringLiteral("控制消息必须是 JSON 对象"));
        return false;
    }
    root = *parsed;
    const Value *version = root.member(u"v");
    if (!version) {
        fail(result, ErrorCode::ProtocolVersion, QStringLiteral("缺少协议版本 v"));
        return false;
    }
    const auto versionValue = version->integer();
    if (!versionValue) {
        fail(result, ErrorCode::ProtocolVersion, QStringLiteral("协议版本 v 必须是整数"));
        return false;
    }
    if (*versionValue != ControlVersion) {
        fail(result, ErrorCode::ProtocolVersion,
             QStringLiteral("不支持的协议版本 %1").arg(*versionValue));
        return false;
    }
    return true;
}

bool readRequestId(const Value &root, quint32 &id, DecodeResult &result)
{
    const Value *value = nullptr;
    if (!requireMember(root, u"id", value, result, QStringLiteral("请求 ID"))) return false;
    const auto integer = value->unsignedInteger();
    if (!integer || *integer == 0 || *integer > MaxRequestId) {
        fail(result, ErrorCode::InvalidRequest,
             QStringLiteral("请求 ID 必须是 1..4294967295 的整数"));
        return false;
    }
    id = quint32(*integer);
    return true;
}

bool requestHasKind(const Value &root, bool &isRequest, bool &isResponse, bool &isEvent,
                    DecodeResult &result)
{
    const bool hasOp = root.member(u"op") != nullptr;
    const bool hasEvent = root.member(u"event") != nullptr;
    const bool hasOk = root.member(u"ok") != nullptr;
    const bool hasError = root.member(u"error") != nullptr;
    const bool hasId = root.member(u"id") != nullptr;
    isRequest = hasOp && !hasEvent && !hasOk && !hasError;
    isEvent = hasEvent && !hasOp && !hasOk && !hasError && !hasId;
    isResponse = hasOk && !hasOp && !hasEvent;
    const int kinds = int(isRequest) + int(isEvent) + int(isResponse);
    if (kinds != 1) {
        fail(result, ErrorCode::InvalidRequest,
             QStringLiteral("控制消息必须且只能是一种请求、响应或事件"));
        return false;
    }
    return true;
}

bool readEmptyData(const Value &data, DecodeResult &result)
{
    if (data.objectSize() != 0) {
        fail(result, ErrorCode::InvalidRequest, QStringLiteral("该操作不接受 data 字段"));
        return false;
    }
    return true;
}

bool decodeRequestData(const Value &data, Request &request, DecodeResult &result)
{
    switch (request.op) {
    case Operation::Hello:
    case Operation::Ping:
        return readEmptyData(data, result);
    case Operation::Create:
        if (!readString(data, u"profile", request.profile, result)) return false;
        if (!profileInfo(request.profile)) {
            fail(result, ErrorCode::InvalidRequest, QStringLiteral("profile 不是已定义档位"));
            return false;
        }
        return true;
    case Operation::Join:
        if (!readString(data, u"roomCode", request.roomCode, result)) return false;
        if (!isValidRoomCode(request.roomCode)) {
            fail(result, ErrorCode::InvalidRequest, QStringLiteral("roomCode 必须是八位十进制字符串"));
            return false;
        }
        return true;
    case Operation::Leave:
    case Operation::Stop:
        return readU64String(data, u"roomId", false, request.roomId, result);
    case Operation::StreamConfigure: {
        if (!readU64String(data, u"roomId", false, request.roomId, result)) return false;
        QString codec;
        if (!readString(data, u"codec", codec, result)) return false;
        if (codec != QStringLiteral("h264") && codec != QStringLiteral("bgra-tiles")) {
            fail(result, ErrorCode::InvalidRequest, QStringLiteral("codec 只接受 h264 或 bgra-tiles"));
            return false;
        }
        request.format.codec = codec;
        if (!readUint(data, u"width", 2, 8192, request.format.width, result)) return false;
        if (!readUint(data, u"height", 2, 8192, request.format.height, result)) return false;
        if (!readUint(data, u"fps", 1, 120, request.format.fps, result)) return false;
        if (!readUint(data, u"bitrate", 1, 100'000'000, request.format.bitrate, result)) return false;
        if (!readHexString(data, u"initializationData", request.format.initializationData, result))
            return false;
        if (codec == QStringLiteral("bgra-tiles") && !request.format.initializationData.isEmpty()) {
            fail(result,ErrorCode::InvalidRequest,QStringLiteral("bgra-tiles 初始化数据必须为空")); return false;
        }
        if (!profileInfoForDimensions(request.format.width, request.format.height,
                                      request.format.fps, request.format.bitrate)) {
            fail(result, ErrorCode::InvalidRequest,
                 QStringLiteral("格式尺寸、帧率与码率必须匹配已定义档位"));
            return false;
        }
        request.format.streamEpoch = 0;
        return true;
    }
    case Operation::StreamReady:
    case Operation::StreamKeyframe:
        if (!readU64String(data, u"roomId", false, request.roomId, result)) return false;
        return readU64String(data, u"streamEpoch", false, request.streamEpoch, result);
    case Operation::StreamFeedback:
        if (!readU64String(data, u"roomId", false, request.roomId, result)) return false;
        if (!readU64String(data, u"streamEpoch", false, request.streamEpoch, result)) return false;
        if (!readU64String(data, u"completePackets", true, request.feedback.completePackets, result))
            return false;
        if (!readU64String(data, u"missingFragments", true, request.feedback.missingFragments, result))
            return false;
        if (!readU64String(data, u"decodedFrames", true, request.feedback.decodedFrames, result))
            return false;
        if (!readU64String(data, u"displayedFrames", true, request.feedback.displayedFrames, result))
            return false;
        if (!readBool(data, u"waiting", request.feedback.waiting, result)) return false;
        return readU64String(data, u"receivedBytes", true, request.feedback.receivedBytes, result);
    }
    fail(result, ErrorCode::UnknownOperation, QStringLiteral("未知操作"));
    return false;
}

bool decodeResponseData(const Value &data, Response &response, DecodeResult &result)
{
    switch (response.op) {
    case Operation::Hello:
        return readHello(data, response.hello, result);
    case Operation::Create:
        return readRoom(data, Role::Publisher, response.room, result);
    case Operation::Join:
        return readRoom(data, Role::Viewer, response.room, result);
    case Operation::Leave:
    case Operation::Stop:
    case Operation::Ping:
    case Operation::StreamFeedback:
        return readEmptyData(data, result);
    case Operation::StreamConfigure:
    case Operation::StreamReady:
    case Operation::StreamKeyframe:
        if (!readU64String(data, u"roomId", false, response.roomId, result)) return false;
        return readU64String(data, u"streamEpoch", false, response.streamEpoch, result);
    }
    fail(result, ErrorCode::UnknownOperation, QStringLiteral("未知响应类型"));
    return false;
}

bool decodeEventData(const Value &data, Event &event, DecodeResult &result)
{
    if (!readU64String(data, u"roomId", false, event.roomId, result)) return false;
    switch (event.kind) {
    case EventKind::RoomState: {
        QString state;
        if (!readString(data, u"state", state, result)) return false;
        const auto parsed = parseRoomState(state);
        if (!parsed) {
            fail(result, ErrorCode::InvalidRequest, QStringLiteral("未知房间状态"));
            return false;
        }
        event.roomState = *parsed;
        return readUint(data, u"viewerCount", 0, MaxViewersPerRoom, event.viewerCount, result);
    }
    case EventKind::StreamConfig: {
        const Value *format = nullptr;
        if (!readObject(data, u"format", format, result)) return false;
        return readFormat(*format, event.format, result);
    }
    case EventKind::StreamKeyframe:
        return readU64String(data, u"streamEpoch", false, event.streamEpoch, result);
    case EventKind::RoomClosed: {
        QString reason;
        if (!readString(data, u"reason", reason, result)) return false;
        const auto parsed = parseCloseReason(reason);
        if (!parsed) {
            fail(result, ErrorCode::InvalidRequest, QStringLiteral("未知关闭原因"));
            return false;
        }
        event.reason = *parsed;
        return true;
    }
    }
    fail(result, ErrorCode::InvalidRequest, QStringLiteral("未知事件类型"));
    return false;
}

} // namespace

QString errorCodeName(ErrorCode code)
{
    switch (code) {
    case ErrorCode::None: return QString();
    case ErrorCode::ProtocolVersion: return QStringLiteral("PROTOCOL_VERSION");
    case ErrorCode::InvalidRequest: return QStringLiteral("INVALID_REQUEST");
    case ErrorCode::UnknownOperation: return QStringLiteral("UNKNOWN_OPERATION");
    case ErrorCode::NotReady: return QStringLiteral("NOT_READY");
    case ErrorCode::AlreadyInRoom: return QStringLiteral("ALREADY_IN_ROOM");
    case ErrorCode::RoomNotFound: return QStringLiteral("ROOM_NOT_FOUND");
    case ErrorCode::RoomFull: return QStringLiteral("ROOM_FULL");
    case ErrorCode::Capacity: return QStringLiteral("CAPACITY");
    case ErrorCode::Forbidden: return QStringLiteral("FORBIDDEN");
    case ErrorCode::StaleStream: return QStringLiteral("STALE_STREAM");
    case ErrorCode::RateLimited: return QStringLiteral("RATE_LIMITED");
    case ErrorCode::UdpUnavailable: return QStringLiteral("UDP_UNAVAILABLE");
    case ErrorCode::MediaUnavailable: return QStringLiteral("MEDIA_UNAVAILABLE");
    case ErrorCode::Timeout: return QStringLiteral("TIMEOUT");
    case ErrorCode::Internal: return QStringLiteral("INTERNAL");
    }
    return QString();
}

std::optional<ErrorCode> parseErrorCode(QStringView name)
{
    for (int i = 1; i <= int(ErrorCode::Internal); ++i) {
        const auto code = ErrorCode(i);
        if (errorCodeName(code) == name) return code;
    }
    return std::nullopt;
}

QString roleName(Role role)
{
    switch (role) {
    case Role::Publisher: return QStringLiteral("publisher");
    case Role::Viewer: return QStringLiteral("viewer");
    }
    return QString();
}

std::optional<Role> parseRole(QStringView name)
{
    if (name == u"publisher") return Role::Publisher;
    if (name == u"viewer") return Role::Viewer;
    return std::nullopt;
}

QString roomStateName(RoomState state)
{
    switch (state) {
    case RoomState::Preparing: return QStringLiteral("preparing");
    case RoomState::Running: return QStringLiteral("running");
    case RoomState::Closed: return QStringLiteral("closed");
    }
    return QString();
}

std::optional<RoomState> parseRoomState(QStringView name)
{
    if (name == u"preparing") return RoomState::Preparing;
    if (name == u"running") return RoomState::Running;
    if (name == u"closed") return RoomState::Closed;
    return std::nullopt;
}

QString closeReasonName(CloseReason reason)
{
    switch (reason) {
    case CloseReason::OwnerStopped: return QStringLiteral("owner_stopped");
    case CloseReason::OwnerDisconnected: return QStringLiteral("owner_disconnected");
    case CloseReason::OwnerLost: return QStringLiteral("owner_lost");
    case CloseReason::StreamFailed: return QStringLiteral("stream_failed");
    case CloseReason::ConfigTimeout: return QStringLiteral("config_timeout");
    case CloseReason::ServerShutdown: return QStringLiteral("server_shutdown");
    }
    return QString();
}

std::optional<CloseReason> parseCloseReason(QStringView name)
{
    if (name == u"owner_stopped") return CloseReason::OwnerStopped;
    if (name == u"owner_disconnected") return CloseReason::OwnerDisconnected;
    if (name == u"owner_lost") return CloseReason::OwnerLost;
    if (name == u"stream_failed") return CloseReason::StreamFailed;
    if (name == u"config_timeout") return CloseReason::ConfigTimeout;
    if (name == u"server_shutdown") return CloseReason::ServerShutdown;
    return std::nullopt;
}

QString operationName(Operation operation)
{
    switch (operation) {
    case Operation::Hello: return QStringLiteral("hello");
    case Operation::Create: return QStringLiteral("create");
    case Operation::Join: return QStringLiteral("join");
    case Operation::Leave: return QStringLiteral("leave");
    case Operation::Stop: return QStringLiteral("stop");
    case Operation::Ping: return QStringLiteral("ping");
    case Operation::StreamConfigure: return QStringLiteral("stream.configure");
    case Operation::StreamReady: return QStringLiteral("stream.ready");
    case Operation::StreamKeyframe: return QStringLiteral("stream.keyframe");
    case Operation::StreamFeedback: return QStringLiteral("stream.feedback");
    }
    return QString();
}

std::optional<Operation> parseOperation(QStringView name)
{
    for (int i = 0; i <= int(Operation::StreamFeedback); ++i) {
        const auto operation = Operation(i);
        if (operationName(operation) == name) return operation;
    }
    return std::nullopt;
}

QString eventName(EventKind kind)
{
    switch (kind) {
    case EventKind::RoomState: return QStringLiteral("room.state");
    case EventKind::StreamConfig: return QStringLiteral("stream.config");
    case EventKind::StreamKeyframe: return QStringLiteral("stream.keyframe");
    case EventKind::RoomClosed: return QStringLiteral("room.closed");
    }
    return QString();
}

std::optional<EventKind> parseEventName(QStringView name)
{
    for (int i = 0; i <= int(EventKind::RoomClosed); ++i) {
        const auto kind = EventKind(i);
        if (eventName(kind) == name) return kind;
    }
    return std::nullopt;
}

const ProfileInfo *profileInfo(QStringView name)
{
    for (const auto &profile : Profiles)
        if (profile.name == name) return &profile;
    return nullptr;
}

const ProfileInfo *profileInfoForDimensions(int width, int height, int fps, int bitrate)
{
    for (const auto &profile : Profiles)
        if (profile.width == width && profile.height == height && profile.fps == fps
            && profile.bitrate == bitrate)
            return &profile;
    return nullptr;
}

QByteArray encodeRequest(const Request &request)
{
    if (request.id == 0) return {};
    json::Writer writer;
    writer.beginObject();
    writer.key(u"v");
    writer.number(ControlVersion);
    writer.key(u"id");
    writer.number(quint64(request.id));
    writer.key(u"op");
    writer.string(operationName(request.op));
    writer.key(u"data");
    writer.beginObject();
    switch (request.op) {
    case Operation::Hello:
    case Operation::Ping:
        break;
    case Operation::Create:
        if (!profileInfo(request.profile)) return {};
        writer.key(u"profile");
        writer.string(request.profile);
        break;
    case Operation::Join:
        if (!isValidRoomCode(request.roomCode)) return {};
        writer.key(u"roomCode");
        writer.string(request.roomCode);
        break;
    case Operation::Leave:
    case Operation::Stop:
        if (request.roomId == 0) return {};
        writeU64String(writer, u"roomId", request.roomId);
        break;
    case Operation::StreamConfigure:
        if (request.roomId == 0 || !writeFormatDimensions(request.format)) return {};
        writeU64String(writer, u"roomId", request.roomId);
        writer.key(u"codec");
        writer.string(request.format.codec);
        writer.key(u"width");
        writer.number(request.format.width);
        writer.key(u"height");
        writer.number(request.format.height);
        writer.key(u"fps");
        writer.number(request.format.fps);
        writer.key(u"bitrate");
        writer.number(request.format.bitrate);
        writer.key(u"initializationData");
        writer.string(QString::fromLatin1(initializationDataToHex(request.format.initializationData)));
        break;
    case Operation::StreamReady:
    case Operation::StreamKeyframe:
        if (request.roomId == 0 || request.streamEpoch == 0) return {};
        writeU64String(writer, u"roomId", request.roomId);
        writeU64String(writer, u"streamEpoch", request.streamEpoch);
        break;
    case Operation::StreamFeedback:
        if (request.roomId == 0 || request.streamEpoch == 0) return {};
        writeU64String(writer, u"roomId", request.roomId);
        writeU64String(writer, u"streamEpoch", request.streamEpoch);
        writeU64String(writer, u"completePackets", request.feedback.completePackets);
        writeU64String(writer, u"missingFragments", request.feedback.missingFragments);
        writeU64String(writer, u"decodedFrames", request.feedback.decodedFrames);
        writeU64String(writer, u"displayedFrames", request.feedback.displayedFrames);
        writer.key(u"waiting");
        writer.boolean(request.feedback.waiting);
        writeU64String(writer, u"receivedBytes", request.feedback.receivedBytes);
        break;
    }
    writer.endObject();
    writer.endObject();
    return writer.take();
}

DecodeResult decodeRequest(QByteArrayView bytes, Request &request)
{
    DecodeResult result;
    Value root;
    if (!decodeEnvelope(bytes, root, result)) return result;
    bool isRequest = false, isResponse = false, isEvent = false;
    if (!requestHasKind(root, isRequest, isResponse, isEvent, result)) return result;
    if (!isRequest) {
        fail(result, ErrorCode::InvalidRequest, QStringLiteral("消息不是请求"));
        return result;
    }
    if (!readRequestId(root, request.id, result)) return result;
    QString opName;
    if (!readString(root, u"op", opName, result)) return result;
    const auto op = parseOperation(opName);
    if (!op) {
        fail(result, ErrorCode::UnknownOperation,
             QStringLiteral("未知操作：%1").arg(opName));
        return result;
    }
    request.op = *op;
    const Value *data = nullptr;
    if (!readObject(root, u"data", data, result)) return result;
    if (!decodeRequestData(*data, request, result)) return result;
    result.ok = true;
    return result;
}

QByteArray encodeResponse(const Response &response)
{
    if (response.id == 0) return {};
    json::Writer writer;
    writer.beginObject();
    writer.key(u"v");
    writer.number(ControlVersion);
    writer.key(u"id");
    writer.number(quint64(response.id));
    writer.key(u"ok");
    writer.boolean(response.ok);
    if (!response.ok) {
        if (response.error == ErrorCode::None) return {};
        writer.key(u"error");
        writer.beginObject();
        writer.key(u"code");
        writer.string(errorCodeName(response.error));
        writer.key(u"message");
        writer.string(response.message);
        writer.endObject();
        writer.endObject();
        return writer.take();
    }
    writer.key(u"data");
    writer.beginObject();
    switch (response.op) {
    case Operation::Hello:
        if (!writeHello(response.hello)) return {};
        writeHelloData(writer, response.hello);
        break;
    case Operation::Create:
        if (response.room.role != Role::Publisher || !writeRoom(response.room)) return {};
        writeRoomData(writer, response.room);
        break;
    case Operation::Join:
        if (response.room.role != Role::Viewer || !writeRoom(response.room)) return {};
        writeRoomData(writer, response.room);
        break;
    case Operation::Leave:
    case Operation::Stop:
    case Operation::Ping:
    case Operation::StreamFeedback:
        break;
    case Operation::StreamConfigure:
    case Operation::StreamReady:
    case Operation::StreamKeyframe:
        if (response.roomId == 0 || response.streamEpoch == 0) return {};
        writeU64String(writer, u"roomId", response.roomId);
        writeU64String(writer, u"streamEpoch", response.streamEpoch);
        break;
    }
    writer.endObject();
    writer.endObject();
    return writer.take();
}

DecodeResult decodeResponse(QByteArrayView bytes, Operation expected, Response &response)
{
    DecodeResult result;
    Value root;
    if (!decodeEnvelope(bytes, root, result)) return result;
    bool isRequest = false, isResponse = false, isEvent = false;
    if (!requestHasKind(root, isRequest, isResponse, isEvent, result)) return result;
    if (!isResponse) {
        fail(result, ErrorCode::InvalidRequest, QStringLiteral("消息不是响应"));
        return result;
    }
    response = {};
    response.op = expected;
    if (!readRequestId(root, response.id, result)) return result;
    const Value *ok = nullptr;
    if (!requireMember(root, u"ok", ok, result, QStringLiteral("响应结果"))) return result;
    if (!ok->isBool()) {
        fail(result, ErrorCode::InvalidRequest, QStringLiteral("ok 必须是布尔值"));
        return result;
    }
    response.ok = ok->boolean();
    const Value *data = root.member(u"data");
    const Value *error = root.member(u"error");
    if (response.ok) {
        if (!data) {
            fail(result, ErrorCode::InvalidRequest, QStringLiteral("成功响应缺少 data"));
            return result;
        }
        if (error) {
            fail(result, ErrorCode::InvalidRequest, QStringLiteral("成功响应不能包含 error"));
            return result;
        }
        if (!data->isObject()) {
            fail(result, ErrorCode::InvalidRequest, QStringLiteral("响应 data 必须是对象"));
            return result;
        }
        if (!decodeResponseData(*data, response, result)) return result;
    } else {
        if (data) {
            fail(result, ErrorCode::InvalidRequest, QStringLiteral("错误响应不能包含 data"));
            return result;
        }
        if (!error || !error->isObject()) {
            fail(result, ErrorCode::InvalidRequest, QStringLiteral("错误响应缺少 error 对象"));
            return result;
        }
        QString codeName;
        if (!readString(*error, u"code", codeName, result)) return result;
        const auto code = parseErrorCode(codeName);
        if (!code || *code == ErrorCode::None) {
            fail(result, ErrorCode::InvalidRequest,
                 QStringLiteral("未知错误码：%1").arg(codeName));
            return result;
        }
        response.error = *code;
        if (!readString(*error, u"message", response.message, result)) return result;
    }
    result.ok = true;
    return result;
}

QByteArray encodeEvent(const Event &event)
{
    json::Writer writer;
    writer.beginObject();
    writer.key(u"v");
    writer.number(ControlVersion);
    writer.key(u"event");
    writer.string(eventName(event.kind));
    writer.key(u"data");
    writer.beginObject();
    if (event.roomId == 0) return {};
    writeU64String(writer, u"roomId", event.roomId);
    switch (event.kind) {
    case EventKind::RoomState:
        if (event.viewerCount < 0 || event.viewerCount > MaxViewersPerRoom) return {};
        writer.key(u"state");
        writer.string(roomStateName(event.roomState));
        writer.key(u"viewerCount");
        writer.number(event.viewerCount);
        break;
    case EventKind::StreamConfig:
        if (!writeFormatDimensions(event.format) || event.format.streamEpoch == 0) return {};
        writer.key(u"format");
        writeFormat(writer, event.format);
        break;
    case EventKind::StreamKeyframe:
        if (event.streamEpoch == 0) return {};
        writeU64String(writer, u"streamEpoch", event.streamEpoch);
        break;
    case EventKind::RoomClosed:
        writer.key(u"reason");
        writer.string(closeReasonName(event.reason));
        break;
    }
    writer.endObject();
    writer.endObject();
    return writer.take();
}

DecodeResult decodeEvent(QByteArrayView bytes, Event &event)
{
    DecodeResult result;
    Value root;
    if (!decodeEnvelope(bytes, root, result)) return result;
    bool isRequest = false, isResponse = false, isEvent = false;
    if (!requestHasKind(root, isRequest, isResponse, isEvent, result)) return result;
    if (!isEvent) {
        fail(result, ErrorCode::InvalidRequest, QStringLiteral("消息不是事件"));
        return result;
    }
    QString name;
    if (!readString(root, u"event", name, result)) return result;
    const auto kind = parseEventName(name);
    if (!kind) {
        fail(result, ErrorCode::InvalidRequest, QStringLiteral("未知事件：%1").arg(name));
        return result;
    }
    event = {};
    event.kind = *kind;
    const Value *data = nullptr;
    if (!readObject(root, u"data", data, result)) return result;
    if (!decodeEventData(*data, event, result)) return result;
    result.ok = true;
    return result;
}

bool isValidRoomCode(QStringView code)
{
    if (code.size() != 8) return false;
    for (const QChar c : code)
        if (!asciiDigit(c)) return false;
    return true;
}

bool isValidTokenHex(QStringView hex)
{
    if (hex.size() != 32) return false;
    for (const QChar c : hex)
        if (!asciiHex(c)) return false;
    return true;
}

QByteArray tokenToHex(QByteArrayView raw)
{
    if (raw.size() != 16) return {};
    return toBytes(raw).toHex();
}

std::optional<QByteArray> tokenFromHex(QStringView hex)
{
    if (!isValidTokenHex(hex)) return std::nullopt;
    return QByteArray::fromHex(hex.toString().toLatin1());
}

bool isValidInitializationData(QStringView hex)
{
    if (hex.size() > MaxInitializationBytes * 2) return false;
    if (hex.size() % 2 != 0) return false;
    for (const QChar c : hex)
        if (!asciiHex(c)) return false;
    return true;
}

std::optional<QByteArray> initializationDataFromHex(QStringView hex)
{
    if (!isValidInitializationData(hex)) return std::nullopt;
    return QByteArray::fromHex(hex.toString().toLatin1());
}

QByteArray initializationDataToHex(QByteArrayView data)
{
    if (data.size() > MaxInitializationBytes) return {};
    return toBytes(data).toHex();
}

bool isValidCounterText(QStringView text)
{
    return parseU64(text).has_value();
}

std::optional<quint64> parseU64(QStringView text)
{
    if (text.isEmpty() || text.size() > 20) return std::nullopt;
    if (text == u"0") return 0;
    if (text[0] < QLatin1Char('1') || text[0] > QLatin1Char('9')) return std::nullopt;
    for (qsizetype i = 1; i < text.size(); ++i)
        if (!asciiDigit(text[i])) return std::nullopt;
    bool ok = false;
    const quint64 value = text.toString().toULongLong(&ok, 10);
    if (!ok) return std::nullopt;
    return value;
}

std::optional<quint32> parseU32(QStringView text)
{
    const auto value = parseU64(text);
    if (!value || *value > MaxRequestId) return std::nullopt;
    return quint32(*value);
}

} // namespace vusiif::zto::desktop_share
