#pragma once
#include "AudioDeviceCache.h"
#include "VoiceAudio.h"
#include <QJsonArray>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QMap>
#include <set>
#include <limits>
namespace vusiif::zto::voice_chat {
class VoiceSession final : public QObject {
    Q_OBJECT
public:
    explicit VoiceSession(std::shared_ptr<AudioDeviceCache> cache, QObject *parent = nullptr);
    ~VoiceSession() override { delete audio; audio=nullptr; }
    void refreshDevices();
    void open(const QString &address, quint16 port, const QString &nickname, const QString &ztid = {});
    void close();
    void stopForUnload() { close(); }
    void create(const QString &name);
    void join(quint32 room);
    void leave();
    void configure(const QByteArray &input, const QByteArray &output, bool allowed);
    void mute(bool value);
    void deafen(bool value) { deafened=value; audio->setDeafened(value && !microphoneTesting); }
    void testMicrophone(bool enabled);
    void inputVolume(int value) { audio->setInputVolume(value/100.0f); }
    void memberVolume(quint32 id,int value) { if(members.count(id)) audio->setMemberVolume(id,value/100.0f); }
    void volume(int value) { audio->setVolume(value / 100.0f); }
#ifdef VOICE_TESTING
    // Test-only entry points for exercising member-set diffs without a server.
    void testBeginRoom(quint32 roomId, quint32 selfId) {
        clearRoom(); room = roomId; session = selfId; baselineValid = false;
    }
    void testRoomSnapshot(const QJsonObject &value) { updateRoom(value); }
#endif
signals:
    void notice(const QString &message);
    void connectionChanged(bool connected);
    void roomsChanged(const QJsonArray &rooms);
    void roomChanged(const QJsonObject &room);
    void busyChanged(bool busy);
    void devicesRefreshed();
    void microphoneTestingChanged(bool enabled);
    void selfJoined();
    void memberJoined(quint32 session);
    void memberLeft(quint32 session);
    void roomReset();
private:
    QTcpSocket *tcp;
    QUdpSocket *udp;
    QTimer *heartbeat, *registration, *watchdog;
    VoiceAudio *audio;
    std::shared_ptr<AudioDeviceCache> deviceCache;
    QByteArray incoming, token, inputId, outputId;
    QString nickname, ztid;
    QMap<int, QString> pending;
    std::set<quint32> members;
    quint32 session = 0, room = 0, sequence = 0;
    quint32 baselineRoom = 0;
    bool baselineValid = false;
    quint16 udpPort = 0;
    int nextRequest = 1, registrationAttempts = 0;
    bool ready = false, udpReady = false, recordAllowed = true, muted = false, roomPending = false;
    bool microphoneTesting=false, deafened=false;
    quint32 testSequence=0;
    quint32 testSender() const { return session?session:std::numeric_limits<quint32>::max(); }
    void retainMembers();
    void request(const QString &operation, const QJsonObject &data = {});
    void read();
    void datagrams();
    void clearRoom();
    void updateRoom(const QJsonObject &value);
    void sendRegistration();
};
}
