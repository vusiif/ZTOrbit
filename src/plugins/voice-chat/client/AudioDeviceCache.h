#pragma once
#include "VoiceConfig.h"
#include "VoiceSoundPlayer.h"
#include <ztmedia/MediaSession.h>
#include <ztmedia/AudioClipClient.h>
#include <zto/ZeroTierService.h>
#include <zto/PluginFiles.h>
#include <memory>
namespace vusiif::zto::voice_chat {
struct VoiceDevice {QByteArray id;QString description;bool isDefault=false;};
struct AudioDeviceSnapshot {bool ready=false;QList<VoiceDevice> inputs,outputs;};
class AudioDeviceCache : public QObject {
public:
    util::ZeroTierService *zeroTier=nullptr; // Borrowed until plugin destruction.
    util::PluginFiles *files=nullptr;
    util::PluginStorage *storage=nullptr;
    VoiceConfig config;
    std::shared_ptr<media::MediaSession> audio=std::make_shared<media::MediaSession>(nullptr);
    std::shared_ptr<media::AudioClipClient> clips;
    std::shared_ptr<VoiceSoundPlayer> sounds;
    AudioDeviceSnapshot snapshot() const { return devices; }
    void refresh(QObject *context,std::function<void()> completed) {
        if(!audio) { if(completed) completed(); return; }
        audio->sources(context,[this,completed](media::Result<media::SourceSnapshot> result) {
            if(!result.ok() && audio->error) audio->error(result.error);
            devices={};devices.ready=result.value.ready;
            for(const auto &source:result.value.sources) {
                VoiceDevice device{QByteArray::fromHex(source.id.toLatin1()),source.description,source.isDefault};
                if(source.kind=="microphone")devices.inputs.append(device);else if(source.kind=="speaker")devices.outputs.append(device);
            }
            if(completed) completed();
        });
    }
private:
    AudioDeviceSnapshot devices;
};
}
