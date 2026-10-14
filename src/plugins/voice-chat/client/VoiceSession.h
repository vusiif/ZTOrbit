#pragma once
#include "AudioEngine.h"
#include "AudioDeviceCache.h"
#include <QJsonArray>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QMap>
#include <set>
namespace vusiif::ztgui::voice_chat {
class VoiceSession final : public QObject {
    Q_OBJECT
public:
    explicit VoiceSession(std::shared_ptr<AudioDeviceCache> cache, QObject *parent = nullptr);
    void refreshDevices();
    void open(const QString &address, quint16 port, const QString &nickname);
    void close();
    void create(const QString &name);
    void join(quint32 room);
    void leave();
    void configure(const QByteArray &input, const QByteArray &output, bool allowed);
    void mute(bool value);
    void deafen(bool value) { audio->setDeafened(value); }
    void inputVolume(int value) { audio->setInputVolume(value/100.0f); }
    void memberVolume(quint32 id,int value) { if(members.count(id)) audio->setMemberVolume(id,value/100.0f); }
    void volume(int value) { audio->setVolume(value / 100.0f); }
signals:
    void notice(const QString &message);
    void connectionChanged(bool connected);
    void roomsChanged(const QJsonArray &rooms);
    void roomChanged(const QJsonObject &room);
    void busyChanged(bool busy);
    void devicesRefreshed();
private:
    QTcpSocket *tcp;
    QUdpSocket *udp;
    QTimer *heartbeat, *registration, *watchdog;
    AudioEngine *audio;
    std::shared_ptr<AudioDeviceCache> deviceCache;
    QByteArray incoming, token, inputId, outputId;
    QString nickname;
    QMap<int, QString> pending;
    std::set<quint32> members;
    quint32 session = 0, room = 0, sequence = 0;
    quint16 udpPort = 0;
    int nextRequest = 1, registrationAttempts = 0;
    bool ready = false, udpReady = false, recordAllowed = true, muted = false, roomPending = false;
    void request(const QString &operation, const QJsonObject &data = {});
    void read();
    void datagrams();
    void clearRoom();
    void updateRoom(const QJsonObject &value);
    void sendRegistration();
};
}
