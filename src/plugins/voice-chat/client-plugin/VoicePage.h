#pragma once
#include <QWidget>
#include <QThread>
#include <functional>
#include <memory>
class QLineEdit;
class QSpinBox;
class QPushButton;
class QListWidget;
class QComboBox;
class QCheckBox;
class QLabel;
class QSlider;
class QMediaDevices;
namespace vusiif::ztgui::voice_chat {
class VoiceSession;
class AudioDeviceCache;
class VoicePage final : public QWidget {
    Q_OBJECT
public:
    VoicePage(QWidget *parent, std::shared_ptr<AudioDeviceCache> cache);
    ~VoicePage() override;
private:
    QThread thread;
    VoiceSession *session;
    std::shared_ptr<AudioDeviceCache> deviceCache;
    QLineEdit *address, *nickname, *roomName;
    QSpinBox *port;
    QPushButton *connection, *createRoom, *joinRoom, *leaveRoom;
    QListWidget *rooms, *members;
    QComboBox *inputs, *outputs;
    QCheckBox *micMute, *earMute;
    QLabel *status, *currentRoom;
    QSlider *volume;
    QMediaDevices *devices;
    bool connected = false, busy = false, inRoom = false, recordAllowed = true;
    bool monitoringDevices = false, deviceRefreshPending = false;
    void updateActions();
    void refreshDevices();
    void requestDeviceRefresh();
    void monitorDevices();
    void configureAudio();
    void save();
    void withPermission(std::function<void()> action);
};
}
