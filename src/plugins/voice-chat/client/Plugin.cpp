#include <ztgui/PluginInterface.h>
#include <ztgui/PluginLifecycle.h>
#include "VoiceController.h"
#include <ztgui/PluginPage.h>
#include "AudioDeviceCache.h"
#include <ztgui/PluginStop.h>
#include <QPointer>
#include <stdexcept>
#include <QFile>
#include <QDebug>
namespace vusiif::ztgui::voice_chat {
class VoiceChatPlugin final : public QObject, public vusiif::ztgui::api::PluginInterface,
    public vusiif::ztgui::api::PluginLifecycleInterface, public vusiif::ztgui::api::PluginStopInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ZTGUI_PLUGIN_IID FILE "plugin.json")
    Q_INTERFACES(vusiif::ztgui::api::PluginInterface vusiif::ztgui::api::PluginLifecycleInterface vusiif::ztgui::api::PluginStopInterface)
    std::shared_ptr<vusiif::ztgui::voice_chat::AudioDeviceCache> devices = std::make_shared<vusiif::ztgui::voice_chat::AudioDeviceCache>();
    QList<QPointer<VoiceController>> controllers;
public:
    vusiif::ztgui::api::CreateTask onCreate(vusiif::ztgui::util::HostServices &host,
                                       const vusiif::ztgui::util::CancellationToken &cancellation) override {
        Q_UNUSED(cancellation);
        auto *access=dynamic_cast<util::ServiceAccess *>(&host);
        if(!access) throw std::runtime_error("Host does not provide plugin services");
        auto *storage=dynamic_cast<util::PluginStorage *>(&host.files());
        if(!storage) throw std::runtime_error("Host does not provide the plugin storage transaction extension; update ZTGUI and rebuild this plugin");
        QString error; auto lease=access->pluginServices().acquire("media",media::SessionService,media::SessionApi,&error);
        if(!lease) throw std::runtime_error(error.toStdString());
        QString clipError;
        auto clipLease=access->pluginServices().acquire("media",media::ClipService,media::ClipApi,&clipError);
        if(!clipLease) throw std::runtime_error(clipError.toStdString());
        devices->audio=std::make_shared<media::MediaSession>(std::move(lease));
        devices->clips=std::make_shared<media::AudioClipClient>(std::move(clipLease));
        devices->zeroTier=&host.zeroTier();
        devices->files=&host.files();
        devices->storage=storage;
        const auto loaded=loadVoiceConfig(*storage);
        devices->config=loaded.config;
        if(!loaded.error.isEmpty()) devices->config.error=loaded.error;
        else if(!loaded.warnings.isEmpty()) devices->config.error=loaded.warnings.join(QStringLiteral("\n"));
        QFile join(QStringLiteral(":/ztgui/voice-chat/sounds/join.wav"));
        QFile leave(QStringLiteral(":/ztgui/voice-chat/sounds/leave.wav"));
        if(!join.open(QIODevice::ReadOnly) || !leave.open(QIODevice::ReadOnly))
            throw std::runtime_error("Bundled voice-chat notification sounds are missing");
        devices->sounds=std::make_shared<VoiceSoundPlayer>(devices->clips, join.readAll(), leave.readAll());
        devices->sounds->setOutputDevice(devices->config.output);
        devices->sounds->setVolume(devices->config.volume);
        devices->sounds->setDeafened(devices->config.deafened);
        return {};
    }
    void requestStop(QObject *context,std::function<void(api::CreateResult)> completed) override {
        for(const auto &controller:controllers) if(controller) controller->stopForUnload();
        if(devices->sounds) devices->sounds->stop();
        auto state=std::make_shared<QPair<int,QString>>(2,QString());
        const auto finish=[state,completed](const QString &error) {
            if(!error.isEmpty() && state->second.isEmpty()) state->second=error;
            if(--state->first==0) completed({state->second.isEmpty(),state->second});
        };
        if(devices->audio)
            devices->audio->close(context,[finish](media::Result<bool> r) { finish(r.ok()?QString():r.error.message); });
        else finish({});
        if(devices->clips)
            devices->clips->close(context,[finish](media::Result<bool> r) { finish(r.ok()?QString():r.error.message); });
        else finish({});
    }
    vusiif::ztgui::api::PluginDefinition definition() override {
        vusiif::ztgui::api::PluginDefinition result;
        result.info = {"voice-chat", QStringLiteral("语音聊天"), "0.3.0"};
        vusiif::ztgui::api::MenuContribution menu;
        menu.id = "rooms"; menu.title = QStringLiteral("语音聊天");
        menu.toolTip = QStringLiteral("连接语音服务器并加入房间");
        menu.createPage = [this,cache = devices](QQmlEngine *engine, QQuickItem *parent) {
            auto *controller = new VoiceController(nullptr, cache);
            controllers.append(controller);
            auto *page = api::createQmlPage(engine, parent, QUrl("qrc:/ztgui/voice-chat/VoicePage.qml"),
                {{"controller", QVariant::fromValue(static_cast<QObject *>(controller))}});
            if (page) controller->setParent(page); else {
                QQmlComponent diagnostic(engine,QUrl("qrc:/ztgui/voice-chat/VoicePage.qml"));
                qWarning().noquote()<<"Voice page creation failed; resource"<<QFile::exists(":/ztgui/voice-chat/VoicePage.qml")<<"status"<<diagnostic.status()<<diagnostic.errorString()<<"imports"<<engine->importPathList();
                delete controller;
            }
            return page;
        };
        result.menus.append(menu);
        return result;
    }
};
} // namespace vusiif::ztgui::voice_chat
#include "Plugin.moc"
