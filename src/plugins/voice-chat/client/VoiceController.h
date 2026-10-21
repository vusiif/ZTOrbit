#pragma once
#include <QObject>
#include <QVariantList>
#include <QHash>
#include <QPointer>
#include <QJsonObject>
#include <QJsonValue>
#include <memory>
#include <functional>
namespace vusiif::ztgui::voice_chat {
class VoiceSession;
class AudioDeviceCache;
class VoiceController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString address READ address WRITE setAddress NOTIFY stateChanged)
    Q_PROPERTY(QString nickname READ nickname WRITE setNickname NOTIFY stateChanged)
    Q_PROPERTY(int inputVolume READ inputVolume WRITE setInputVolume NOTIFY stateChanged)
    Q_PROPERTY(int outputVolume READ volume WRITE setVolume NOTIFY stateChanged)
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY stateChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY stateChanged)
    Q_PROPERTY(bool deafened READ deafened WRITE setDeafened NOTIFY stateChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool microphoneTesting READ microphoneTesting NOTIFY stateChanged)
    Q_PROPERTY(bool inRoom READ inRoom NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString currentRoom READ currentRoom NOTIFY stateChanged)
    Q_PROPERTY(QVariantList rooms READ rooms NOTIFY roomsChanged)
    Q_PROPERTY(QVariantList members READ members NOTIFY stateChanged)
    Q_PROPERTY(QVariantList inputs READ inputs NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY devicesChanged)
    Q_PROPERTY(QString input READ input WRITE setInput NOTIFY devicesChanged)
    Q_PROPERTY(QString output READ output WRITE setOutput NOTIFY devicesChanged)
public:
    explicit VoiceController(QObject *parent, std::shared_ptr<AudioDeviceCache> cache);
    ~VoiceController() override;
    void stopForUnload();
    QString address() const { return m_address; }
    QString nickname() const { return m_nickname; }
    int inputVolume() const { return m_inputVolume; }
    int volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    bool deafened() const { return m_deafened; }
    bool connected() const { return m_connected; }
    bool busy() const { return m_busy; }
    bool microphoneTesting() const { return m_microphoneTesting; }
    bool inRoom() const { return m_inRoom; }
    QString status() const { return m_status; }
    QString currentRoom() const { return m_currentRoom; }
    QVariantList rooms() const { return m_rooms; }
    QVariantList members() const { return m_members; }
    QVariantList inputs() const { return m_inputs; }
    QVariantList outputs() const { return m_outputs; }
    QString input() const { return QString::fromLatin1(m_input.toHex()); }
    QString output() const { return QString::fromLatin1(m_output.toHex()); }
    void setAddress(const QString &value);
    void setNickname(const QString &value);
    void setVolume(int value);
    void setInputVolume(int value);
    Q_INVOKABLE void setMemberVolume(quint32 member,int value);
    Q_INVOKABLE int memberVolume(quint32 member) const { return m_memberVolumes.value(member,100); }
    Q_INVOKABLE void setMemberRemark(quint32 member, const QString &remark);
    void setMuted(bool value);
    void setDeafened(bool value);
    void setInput(const QString &value);
    void setOutput(const QString &value);
    Q_INVOKABLE void toggleConnection();
    Q_INVOKABLE void setMicrophoneTesting(bool enabled);
    Q_INVOKABLE void createRoom(const QString &name);
    Q_INVOKABLE void joinRoom(quint32 id);
    Q_INVOKABLE void leaveRoom();
signals:
    void stateChanged();
    void roomsChanged();
    void devicesChanged();
private:
    VoiceSession *m_session;
    std::shared_ptr<AudioDeviceCache> m_cache;
    QString m_address, m_nickname, m_status = QStringLiteral("未连接"), m_currentRoom = QStringLiteral("尚未加入房间");
    int m_volume = 80, m_inputVolume = 100;
    QHash<quint32,int> m_memberVolumes;
    bool m_connected = false, m_busy = false, m_inRoom = false, m_muted = false, m_deafened = false, m_recordAllowed = true;
    bool m_monitoring = false, m_refreshPending = false;
    bool m_stopping=false;
    bool m_microphoneTesting=false, m_testRequested=false;
    QPointer<QObject> m_identityRequest;
    QByteArray m_input, m_output;
    QVariantList m_rooms, m_members, m_inputs, m_outputs;
    QJsonObject m_remarks;
    bool m_remarksWritable=true;
    void updateMemberNames();
    bool persistField(const QString &key, const QJsonValue &value);
    void configureAudio();
    void refreshDevices();
    void requestDeviceRefresh();
    void monitorDevices();
    void withPermission(std::function<void()> action);
};
}
