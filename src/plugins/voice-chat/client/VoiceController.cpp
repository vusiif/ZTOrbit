#include "VoiceController.h"
#include "VoiceSession.h"
#include <QCoreApplication>
#include <QMediaDevices>
#include <QPermissions>
#include <QSettings>
#include <ztprotocol/NetworkAddress.h>
#include <algorithm>

namespace vusiif::ztgui::voice_chat {
VoiceController::VoiceController(QObject *parent, std::shared_ptr<AudioDeviceCache> cache)
    : QObject(parent), m_session(new VoiceSession(cache)), m_cache(std::move(cache)), m_devices(new QMediaDevices(this))
{
    QSettings settings;
    m_address = settings.value("voice-chat/address").toString(); m_nickname = settings.value("voice-chat/nickname").toString();
    m_volume = std::clamp(settings.value("voice-chat/volume", 80).toInt(), 0, 100);
    m_muted = settings.value("voice-chat/muted", false).toBool(); m_deafened = settings.value("voice-chat/deafened", false).toBool();
    m_input = settings.value("voice-chat/input").toByteArray(); m_output = settings.value("voice-chat/output").toByteArray();
    refreshDevices();
    m_session->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_session, &QObject::deleteLater);
    connect(m_session, &VoiceSession::notice, this, [this](const QString &message) { m_status = message; emit stateChanged(); });
    connect(m_session, &VoiceSession::connectionChanged, this, [this](bool value) { m_connected = value; emit stateChanged(); });
    connect(m_session, &VoiceSession::busyChanged, this, [this](bool value) { m_busy = value; emit stateChanged(); });
    connect(m_session, &VoiceSession::devicesRefreshed, this, [this] { m_refreshPending = false; refreshDevices(); monitorDevices(); configureAudio(); });
    connect(m_session, &VoiceSession::roomsChanged, this, [this](const QJsonArray &list) { m_rooms = list.toVariantList(); emit roomsChanged(); });
    connect(m_session, &VoiceSession::roomChanged, this, [this](const QJsonObject &value) {
        m_inRoom = !value.isEmpty(); m_currentRoom = m_inRoom ? value["name"].toString() : QStringLiteral("尚未加入房间");
        m_members = value["members"].toArray().toVariantList(); emit stateChanged();
    });
    m_thread.start(); configureAudio();
    if (m_cache->snapshot().ready) monitorDevices();
    requestDeviceRefresh(); setMuted(m_muted); setDeafened(m_deafened); setVolume(m_volume);
}
VoiceController::~VoiceController()
{
    save();
    if (m_thread.isRunning()) {
        QMetaObject::invokeMethod(m_session, [worker = m_session] { worker->close(); }, Qt::BlockingQueuedConnection);
        m_thread.quit(); m_thread.wait();
    }
}
void VoiceController::setAddress(const QString &value) { if (m_connected || m_busy) return; m_address = value; save(); emit stateChanged(); }
void VoiceController::setNickname(const QString &value) { if (m_connected || m_busy) return; m_nickname = value.left(64); save(); emit stateChanged(); }
void VoiceController::setVolume(int value)
{
    m_volume = std::clamp(value, 0, 100); save(); emit stateChanged();
    QMetaObject::invokeMethod(m_session, [worker = m_session, value = m_volume] { worker->volume(value); });
}
void VoiceController::setMuted(bool value) { m_muted = value; save(); emit stateChanged(); QMetaObject::invokeMethod(m_session, [worker = m_session, value] { worker->mute(value); }); }
void VoiceController::setDeafened(bool value) { m_deafened = value; save(); emit stateChanged(); QMetaObject::invokeMethod(m_session, [worker = m_session, value] { worker->deafen(value); }); }
void VoiceController::setInput(const QString &value) { m_input = QByteArray::fromHex(value.toLatin1()); configureAudio(); save(); emit devicesChanged(); }
void VoiceController::setOutput(const QString &value) { m_output = QByteArray::fromHex(value.toLatin1()); configureAudio(); save(); emit devicesChanged(); }
void VoiceController::toggleConnection()
{
    if (m_connected || m_busy) { QMetaObject::invokeMethod(m_session, [worker = m_session] { worker->close(); }); m_status = QStringLiteral("已断开"); emit stateChanged(); return; }
    const auto endpoint = protocol::serverEndpoint(m_address);
    const auto name = m_nickname.trimmed();
    if (!endpoint.valid() || name.isEmpty()) { m_status = QStringLiteral("请填写有效的服务器地址和昵称（可使用 地址:端口）"); emit stateChanged(); return; }
    save(); m_busy = true; emit stateChanged();
    QMetaObject::invokeMethod(m_session, [worker = m_session, endpoint, name] { worker->open(endpoint.host, endpoint.port, name); });
}
void VoiceController::createRoom(const QString &input)
{
    if (!m_connected || m_inRoom || m_busy) return;
    const auto name = input.trimmed().left(64);
    if (name.isEmpty()) { m_status = QStringLiteral("请填写房间名称"); emit stateChanged(); return; }
    withPermission([this, name] { m_busy = true; emit stateChanged(); QMetaObject::invokeMethod(m_session, [worker = m_session, name] { worker->create(name); }); });
}
void VoiceController::joinRoom(quint32 id)
{
    if (!m_connected || m_inRoom || m_busy || !id) return;
    withPermission([this, id] { m_busy = true; emit stateChanged(); QMetaObject::invokeMethod(m_session, [worker = m_session, id] { worker->join(id); }); });
}
void VoiceController::leaveRoom()
{
    if (!m_connected || !m_inRoom || m_busy) return;
    m_busy = true; emit stateChanged(); QMetaObject::invokeMethod(m_session, [worker = m_session] { worker->leave(); });
}
void VoiceController::configureAudio()
{
    QMetaObject::invokeMethod(m_session, [worker = m_session, input = m_input, output = m_output, allowed = m_recordAllowed] { worker->configure(input, output, allowed); });
}
void VoiceController::refreshDevices()
{
    const auto snapshot = m_cache->snapshot();
    const auto fill = [](const QList<QAudioDevice> &devices, QByteArray &selected) {
        QVariantList result{QVariantMap{{"label", QStringLiteral("系统默认")}, {"id", QString()}}};
        bool found = selected.isEmpty();
        for (const auto &device : devices) { result.append(QVariantMap{{"label", device.description()}, {"id", QString::fromLatin1(device.id().toHex())}}); found |= device.id() == selected; }
        if (!found) selected.clear();
        return result;
    };
    m_inputs = fill(snapshot.inputs, m_input); m_outputs = fill(snapshot.outputs, m_output); emit devicesChanged();
}
void VoiceController::requestDeviceRefresh()
{
    if (m_refreshPending) return;
    m_refreshPending = true; QMetaObject::invokeMethod(m_session, [worker = m_session] { worker->refreshDevices(); });
}
void VoiceController::monitorDevices()
{
    if (m_monitoring) return;
    m_monitoring = true;
    connect(m_devices, &QMediaDevices::audioInputsChanged, this, [this] { requestDeviceRefresh(); m_status = QStringLiteral("输入设备已变化，请检查设备选择"); emit stateChanged(); });
    connect(m_devices, &QMediaDevices::audioOutputsChanged, this, [this] { requestDeviceRefresh(); m_status = QStringLiteral("输出设备已变化，请检查设备选择"); emit stateChanged(); });
}
void VoiceController::save()
{
    QSettings settings;
    settings.setValue("voice-chat/address", m_address.trimmed()); settings.remove("voice-chat/port");
    settings.setValue("voice-chat/nickname", m_nickname.trimmed()); settings.setValue("voice-chat/input", m_input); settings.setValue("voice-chat/output", m_output);
    settings.setValue("voice-chat/volume", m_volume); settings.setValue("voice-chat/muted", m_muted); settings.setValue("voice-chat/deafened", m_deafened);
}
void VoiceController::withPermission(std::function<void()> action)
{
#ifdef Q_OS_MACOS
    const QMicrophonePermission permission;
    const auto state = qApp->checkPermission(permission);
    if (state == Qt::PermissionStatus::Undetermined) {
        qApp->requestPermission(permission, this, [this, action](const QPermission &result) { m_recordAllowed = result.status() == Qt::PermissionStatus::Granted; configureAudio(); action(); }); return;
    }
    m_recordAllowed = state == Qt::PermissionStatus::Granted;
#endif
    configureAudio(); action();
}
}
