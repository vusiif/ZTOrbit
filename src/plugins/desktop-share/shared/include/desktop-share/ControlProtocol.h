#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <array>
#include <cstdint>
#include <optional>

namespace vusiif::zto::desktop_share {

inline constexpr char PluginId[] = "desktop-share";
inline constexpr char DisplayName[] = "桌面共享";
inline constexpr char Route[] = "desktop-share";

inline constexpr int ControlVersion = 1;
inline constexpr int MaxControlBytes = 64 * 1024;
inline constexpr int MaxInitializationBytes = 8 * 1024;
inline constexpr int DefaultPort = 18100;
inline constexpr int MaxViewersPerRoom = 8;
inline constexpr int MaxRooms = 8;
inline constexpr int MaxDesktopSessions = 72;

// Error codes are part of the wire contract. Clients switch on the code; the
// message is display-only.
enum class ErrorCode : quint8 {
    None = 0,
    ProtocolVersion,
    InvalidRequest,
    UnknownOperation,
    NotReady,
    AlreadyInRoom,
    RoomNotFound,
    RoomFull,
    Capacity,
    Forbidden,
    StaleStream,
    RateLimited,
    UdpUnavailable,
    MediaUnavailable,
    Timeout,
    Internal,
};

QString errorCodeName(ErrorCode code);
std::optional<ErrorCode> parseErrorCode(QStringView name);

enum class Role : quint8 { Publisher = 0, Viewer = 1 };
QString roleName(Role role);
std::optional<Role> parseRole(QStringView name);

enum class RoomState : quint8 { Preparing = 0, Running = 1, Closed = 2 };
QString roomStateName(RoomState state);
std::optional<RoomState> parseRoomState(QStringView name);

enum class CloseReason : quint8 {
    OwnerStopped = 0,
    OwnerDisconnected,
    OwnerLost,
    StreamFailed,
    ConfigTimeout,
    ServerShutdown,
};
QString closeReasonName(CloseReason reason);
std::optional<CloseReason> parseCloseReason(QStringView name);

enum class Operation : quint8 {
    Hello = 0,
    Create,
    Join,
    Leave,
    Stop,
    Ping,
    StreamConfigure,
    StreamReady,
    StreamKeyframe,
    StreamFeedback,
};
QString operationName(Operation operation);
std::optional<Operation> parseOperation(QStringView name);

enum class EventKind : quint8 { RoomState = 0, StreamConfig, StreamKeyframe, RoomClosed };
QString eventName(EventKind kind);
std::optional<EventKind> parseEventName(QStringView name);

struct ProfileInfo {
    QStringView name;
    int width;
    int height;
    int fps;
    int bitrate;
};

const ProfileInfo *profileInfo(QStringView name);
const ProfileInfo *profileInfoForDimensions(int width, int height, int fps, int bitrate);

struct StreamFormat {
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrate = 0;
    QByteArray initializationData;
    quint64 streamEpoch = 0;
    QString codec = QStringLiteral("h264");
};

struct Feedback {
    quint64 completePackets = 0;
    quint64 missingFragments = 0;
    quint64 decodedFrames = 0;
    quint64 displayedFrames = 0;
    bool waiting = false;
    quint64 receivedBytes = 0;
};

struct Request {
    quint32 id = 0;
    Operation op = Operation::Hello;
    QString profile;
    QString roomCode;
    quint64 roomId = 0;
    StreamFormat format;
    quint64 streamEpoch = 0;
    Feedback feedback;
};

struct HelloInfo {
    quint64 sessionId = 0;
    quint16 udpPort = 0;
    QStringList profiles;
    int maxViewers = 0;
};

struct RoomInfo {
    quint64 roomId = 0;
    QString roomCode;
    QString profile;
    Role role = Role::Publisher;
    QByteArray token;
    bool hasFormat = false;
    StreamFormat format;
};

struct Response {
    quint32 id = 0;
    bool ok = false;
    // Correlation only; not present on the wire.
    Operation op = Operation::Hello;
    HelloInfo hello;
    RoomInfo room;
    quint64 roomId = 0;
    quint64 streamEpoch = 0;
    ErrorCode error = ErrorCode::None;
    QString message;
};

struct Event {
    EventKind kind = EventKind::RoomState;
    quint64 roomId = 0;
    RoomState roomState = RoomState::Preparing;
    int viewerCount = 0;
    StreamFormat format;
    quint64 streamEpoch = 0;
    CloseReason reason = CloseReason::OwnerStopped;
};

struct DecodeResult {
    bool ok = false;
    ErrorCode error = ErrorCode::None;
    QString message;
};

// Canonical compact UTF-8 JSON. Encoding rejects invalid field combinations and
// returns an empty QByteArray; decoding validates before allocating or acting.
QByteArray encodeRequest(const Request &request);
DecodeResult decodeRequest(QByteArrayView bytes, Request &request);

QByteArray encodeResponse(const Response &response);
DecodeResult decodeResponse(QByteArrayView bytes, Operation expected, Response &response);

QByteArray encodeEvent(const Event &event);
DecodeResult decodeEvent(QByteArrayView bytes, Event &event);

bool isValidRoomCode(QStringView code);
bool isValidTokenHex(QStringView hex);
QByteArray tokenToHex(QByteArrayView raw);
std::optional<QByteArray> tokenFromHex(QStringView hex);
bool isValidInitializationData(QStringView hex);
std::optional<QByteArray> initializationDataFromHex(QStringView hex);
QByteArray initializationDataToHex(QByteArrayView data);
bool isValidCounterText(QStringView text);
std::optional<quint64> parseU64(QStringView text);
std::optional<quint32> parseU32(QStringView text);

} // namespace vusiif::zto::desktop_share
