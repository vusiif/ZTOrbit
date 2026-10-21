#include "VoiceController.h"
#include "VoiceConfig.h"
#include "VoiceSession.h"
#include <QCoreApplication>
#include <QPermissions>
#include <QRegularExpression>
#include <QJsonDocument>
#include <ztprotocol/NetworkAddress.h>
#include <algorithm>

namespace vusiif::ztgui::voice_chat {
VoiceController::VoiceController(QObject *parent, std::shared_ptr<AudioDeviceCache> cache)
    : QObject(parent), m_session(new VoiceSession(cache,this)), m_cache(std::move(cache))
{
    const auto &config = m_cache->config;
    m_address = config.address; m_nickname = config.nickname;
    m_volume = config.volume; m_inputVolume = config.inputVolume;
    m_muted = config.muted; m_deafened = config.deafened;
    m_input = config.input; m_output = config.output;
    if (!config.error.isEmpty()) m_status = config.error;
    if(m_cache->files) {
        const auto saved=m_cache->files->readFile("member-remarks.json");
        if(saved.ok() && !saved.data.isEmpty()) {
            QJsonParseError error; const auto doc=QJsonDocument::fromJson(saved.data,&error);
            if(error.error==QJsonParseError::NoError && doc.isObject()) m_remarks=doc.object();
            else { m_remarksWritable=false; m_status=QStringLiteral("成员备注文件损坏，已保留原文件，请检查后重新加载插件。"); }
        }
    }
    refreshDevices();
    connect(m_session, &VoiceSession::notice, this, [this](const QString &message) { m_status = message; emit stateChanged(); });
    connect(m_session, &VoiceSession::connectionChanged, this, [this](bool value) { m_connected = value; emit stateChanged(); });
    connect(m_session, &VoiceSession::busyChanged, this, [this](bool value) { m_busy = value; emit stateChanged(); });
    connect(m_session, &VoiceSession::microphoneTestingChanged, this, [this](bool value) { m_microphoneTesting=value; m_testRequested=value; emit stateChanged(); });
    connect(m_session, &VoiceSession::devicesRefreshed, this, [this] { m_refreshPending = false; refreshDevices(); monitorDevices(); });
    connect(m_session, &VoiceSession::roomsChanged, this, [this](const QJsonArray &list) { m_rooms = list.toVariantList(); emit roomsChanged(); });
    connect(m_session, &VoiceSession::roomChanged, this, [this](const QJsonObject &value) {
        m_inRoom = !value.isEmpty(); m_currentRoom = m_inRoom ? value["name"].toString() : QStringLiteral("尚未加入房间");
        if(!m_inRoom) m_memberVolumes.clear();
        m_members = value["members"].toArray().toVariantList();
        QSet<quint32> present;
        for(auto &v:m_members) { auto row=v.toMap(); const auto id=quint32(row["id"].toUInt()); present.insert(id); row["volume"]=memberVolume(id); v=row; }
        for(auto i=m_memberVolumes.begin();i!=m_memberVolumes.end();) i=present.contains(i.key())?++i:m_memberVolumes.erase(i);
        updateMemberNames();
        emit stateChanged();
    });
    if(m_cache->sounds) {
        connect(m_session,&VoiceSession::selfJoined,this,[this] { m_cache->sounds->enqueue(VoiceSoundPlayer::Cue::Join); });
        connect(m_session,&VoiceSession::memberJoined,this,[this](quint32) { m_cache->sounds->enqueue(VoiceSoundPlayer::Cue::Join); });
        connect(m_session,&VoiceSession::memberLeft,this,[this](quint32) { m_cache->sounds->enqueue(VoiceSoundPlayer::Cue::Leave); });
        connect(m_session,&VoiceSession::roomReset,this,[this] { m_cache->sounds->reset(); });
        m_cache->sounds->setOutputDevice(m_output);
        m_cache->sounds->setVolume(m_volume);
        m_cache->sounds->setDeafened(m_deafened);
    }
    configureAudio();
    if (m_cache->snapshot().ready) monitorDevices();
    requestDeviceRefresh();
    // Seed the session with persisted state without writing the config again.
    QMetaObject::invokeMethod(m_session, [worker = m_session, muted = m_muted, deafened = m_deafened,
                                           volume = m_volume, inputVolume = m_inputVolume] {
        worker->mute(muted); worker->deafen(deafened); worker->volume(volume); worker->inputVolume(inputVolume);
    });
}
VoiceController::~VoiceController()
{
    stopForUnload();
    m_cache->audio->packet={}; m_cache->audio->error={}; m_cache->audio->sourcesChanged={};
}
void VoiceController::stopForUnload() { m_stopping=true; m_testRequested=false; delete m_identityRequest.data(); m_session->stopForUnload(); if(m_cache->sounds) m_cache->sounds->stop(); m_busy=false; m_connected=false; emit stateChanged(); }
bool VoiceController::persistField(const QString &key, const QJsonValue &value)
{
    if(m_stopping) return false;
    if(!m_cache->storage) {
        m_status=QStringLiteral("宿主未提供插件存储事务接口，无法保存语音配置");
        emit stateChanged(); return false;
    }
    QString error;
    if(!saveVoiceConfigField(*m_cache->storage,key,value,error)) {
        m_status=QStringLiteral("无法保存语音配置：%1").arg(error);
        emit stateChanged(); return false;
    }
    return true;
}
void VoiceController::setAddress(const QString &value) {
    if (m_connected || m_busy) return;
    const auto normalized=value.trimmed();
    if(normalized==m_address) return;
    if(!persistField("address",normalized)) return;
    m_address = normalized; emit stateChanged();
}
void VoiceController::setNickname(const QString &value) {
    if (m_connected || m_busy) return;
    const auto normalized=value.left(64);
    if(normalized==m_nickname) return;
    if(!persistField("nickname",normalized)) return;
    m_nickname = normalized; emit stateChanged();
}
void VoiceController::setVolume(int value)
{
    value=std::clamp(value,0,200);
    if(value==m_volume) return;
    if(!persistField("volume",value)) return;
    m_volume = value; if(m_cache->sounds) m_cache->sounds->setVolume(value); emit stateChanged();
    QMetaObject::invokeMethod(m_session, [worker = m_session, value = m_volume] { worker->volume(value); });
}
void VoiceController::setInputVolume(int value) {
    value=std::clamp(value,0,200);
    if(value==m_inputVolume) return;
    if(!persistField("inputVolume",value)) return;
    m_inputVolume=value; emit stateChanged();
    QMetaObject::invokeMethod(m_session,[worker=m_session,value=m_inputVolume] { worker->inputVolume(value); });
}
void VoiceController::setMemberVolume(quint32 member,int value) {
    if(!m_inRoom) return;
    bool found=false; value=std::clamp(value,0,200);
    for(auto &v:m_members) { auto row=v.toMap(); if(row["id"].toUInt()!=member || row["self"].toBool()) continue; row["volume"]=value; v=row; found=true; }
    if(!found) return; m_memberVolumes[member]=value; emit stateChanged();
    QMetaObject::invokeMethod(m_session,[worker=m_session,member,value] { worker->memberVolume(member,value); });
}
void VoiceController::updateMemberNames() {
    static const QRegularExpression identity("\\A[0-9a-f]{10}\\z");
    for(auto &value:m_members) {
        auto row=value.toMap(); auto ztid=row["ztid"].toString().trimmed().toLower();
        // Older servers use the ZT ID as nickname when no nickname was supplied.
        if(!identity.match(ztid).hasMatch()) { ztid=row["nickname"].toString().trimmed().toLower(); if(!identity.match(ztid).hasMatch()) ztid.clear(); }
        const auto remark=m_remarks[ztid].toString(); const auto nickname=row["nickname"].toString().trimmed();
        row["ztid"]=ztid; row["remark"]=remark;
        row["displayName"]=!remark.isEmpty()?remark:!nickname.isEmpty()?nickname:ztid;
        row["canRemark"]=!row["self"].toBool() && !ztid.isEmpty() && m_cache->files && m_remarksWritable;
        value=row;
    }
}
void VoiceController::setMemberRemark(quint32 member, const QString &remark) {
    if(m_stopping || !m_inRoom || !m_cache->storage || !m_remarksWritable) return;
    for(const auto &value:m_members) {
        const auto row=value.toMap(); if(row["id"].toUInt()!=member || !row["canRemark"].toBool()) continue;
        const auto id=row["ztid"].toString(); QJsonObject updated; QString error;
        // Only this ZeroTier ID changes; concurrent remarks from another user survive.
        if(!updateMemberRemark(*m_cache->storage,id,remark,updated,error)) {
            m_remarksWritable=false;
            m_status=QStringLiteral("无法保存成员备注：%1").arg(error); emit stateChanged(); return;
        }
        m_remarks=updated; updateMemberNames(); emit stateChanged(); return;
    }
}
void VoiceController::setMuted(bool value) { if(value==m_muted) return; if(!persistField("muted",value)) m_status=QStringLiteral("麦克风状态已应用，但保存失败"); m_muted = value; emit stateChanged(); QMetaObject::invokeMethod(m_session, [worker = m_session, value] { worker->mute(value); }); }
void VoiceController::setDeafened(bool value) {
    if(value==m_deafened) return;
    if(!persistField("deafened",value)) m_status=QStringLiteral("禁听状态已应用，但保存失败");
    m_deafened = value;
    if(m_cache->sounds) m_cache->sounds->setDeafened(value);
    emit stateChanged();
    QMetaObject::invokeMethod(m_session, [worker = m_session, value] { worker->deafen(value); });
}
void VoiceController::setInput(const QString &value) {
    const auto bytes=QByteArray::fromHex(value.toLatin1());
    if(bytes==m_input) return;
    if(!persistField("input",QString::fromLatin1(bytes.toHex()))) return;
    m_input = bytes; configureAudio(); emit devicesChanged();
}
void VoiceController::setOutput(const QString &value) {
    const auto bytes=QByteArray::fromHex(value.toLatin1());
    if(bytes==m_output) return;
    if(!persistField("output",QString::fromLatin1(bytes.toHex()))) return;
    m_output = bytes;
    if(m_cache->sounds) m_cache->sounds->setOutputDevice(bytes);
    configureAudio(); emit devicesChanged();
}
void VoiceController::toggleConnection()
{
    if(m_stopping) return;
    if (m_connected || m_busy) { delete m_identityRequest.data(); QMetaObject::invokeMethod(m_session, [worker = m_session] { worker->close(); }); m_status = QStringLiteral("已断开"); emit stateChanged(); return; }
    const auto endpoint = protocol::serverEndpoint(m_address);
    const auto name = m_nickname.trimmed();
    if (!endpoint.valid()) { m_status = QStringLiteral("请填写有效的服务器地址（可使用 地址:端口）"); emit stateChanged(); return; }
    if(m_cache->zeroTier) {
        m_busy=true; m_status=QStringLiteral("正在读取 ZeroTier ID…"); emit stateChanged();
        m_identityRequest=new QObject(this);
        m_cache->zeroTier->getZTInfo(m_identityRequest,[this,endpoint,name](const util::JsonResult &result) {
            m_identityRequest->deleteLater(); m_identityRequest=nullptr;
            static const QRegularExpression identity("\\A[0-9a-f]{10}\\z");
            auto id=result.document.object()["address"].toString().trimmed().toLower();
            if(!result.ok() || !identity.match(id).hasMatch()) id.clear();
            if(name.isEmpty() && id.isEmpty()) { m_busy=false; m_status=QStringLiteral("无法读取 ZeroTier ID，请填写昵称后重试"); emit stateChanged(); return; }
            if(name.isEmpty()) m_nickname=id;
            if(name.isEmpty()) persistField("nickname",id);
            emit stateChanged(); m_session->open(endpoint.host,endpoint.port,name.isEmpty()?id:name,id);
        });
        return;
    }
    if(name.isEmpty()) { m_status=QStringLiteral("无法读取 ZeroTier ID，请填写昵称后重试"); emit stateChanged(); return; }
    m_busy = true; emit stateChanged();
    QMetaObject::invokeMethod(m_session, [worker = m_session, endpoint, name] { worker->open(endpoint.host, endpoint.port, name); });
}
void VoiceController::setMicrophoneTesting(bool enabled)
{
    m_testRequested=enabled && !m_stopping;
    if(!m_testRequested) { m_session->testMicrophone(false); return; }
    withPermission([this] {
        if(!m_testRequested) return;
        if(!m_recordAllowed) { m_testRequested=false; m_status=QStringLiteral("未授予麦克风权限，无法测试"); emit stateChanged(); return; }
        m_session->testMicrophone(true);
    });
}
void VoiceController::createRoom(const QString &input)
{
    if (m_stopping || !m_connected || m_inRoom || m_busy) return;
    const auto name = input.trimmed().left(64);
    if (name.isEmpty()) { m_status = QStringLiteral("请填写房间名称"); emit stateChanged(); return; }
    withPermission([this, name] { m_busy = true; emit stateChanged(); QMetaObject::invokeMethod(m_session, [worker = m_session, name] { worker->create(name); }); });
}
void VoiceController::joinRoom(quint32 id)
{
    if (m_stopping || !m_connected || m_inRoom || m_busy || !id) return;
    withPermission([this, id] { m_busy = true; emit stateChanged(); QMetaObject::invokeMethod(m_session, [worker = m_session, id] { worker->join(id); }); });
}
void VoiceController::leaveRoom()
{
    if (m_stopping || !m_connected || !m_inRoom || m_busy) return;
    m_busy = true; emit stateChanged(); QMetaObject::invokeMethod(m_session, [worker = m_session] { worker->leave(); });
}
void VoiceController::configureAudio()
{
    if(m_stopping) return;
    QMetaObject::invokeMethod(m_session, [worker = m_session, input = m_input, output = m_output, allowed = m_recordAllowed] { worker->configure(input, output, allowed); });
}
void VoiceController::refreshDevices()
{
    const auto snapshot = m_cache->snapshot();
    const auto fill = [ready=snapshot.ready](const QList<VoiceDevice> &devices, QByteArray &selected) {
        QVariantList result{QVariantMap{{"label", QStringLiteral("系统默认")}, {"id", QString()}}};
        bool found = selected.isEmpty();
        for (const auto &device : devices) { result.append(QVariantMap{{"label", device.description}, {"id", QString::fromLatin1(device.id.toHex())}}); found |= device.id == selected; }
        if (ready && !found) selected.clear();
        return result;
    };
    m_inputs = fill(snapshot.inputs, m_input); m_outputs = fill(snapshot.outputs, m_output); emit devicesChanged();
}
void VoiceController::requestDeviceRefresh()
{
    if (m_stopping || m_refreshPending) return;
    m_refreshPending = true; QMetaObject::invokeMethod(m_session, [worker = m_session] { worker->refreshDevices(); });
}
void VoiceController::monitorDevices()
{
    if (m_monitoring) return;
    m_monitoring = true;
    m_cache->audio->sourcesChanged=[this] { requestDeviceRefresh(); m_status=QStringLiteral("音频设备已变化，请检查设备选择"); emit stateChanged(); };
}
void VoiceController::withPermission(std::function<void()> action)
{
    if(m_stopping) return;
#ifdef Q_OS_MACOS
    const QMicrophonePermission permission;
    const auto state = qApp->checkPermission(permission);
    if (state == Qt::PermissionStatus::Undetermined) {
        qApp->requestPermission(permission, this, [this, action](const QPermission &result) { if(m_stopping) return; m_recordAllowed = result.status() == Qt::PermissionStatus::Granted; configureAudio(); action(); }); return;
    }
    m_recordAllowed = state == Qt::PermissionStatus::Granted;
#endif
    configureAudio(); action();
}
}
