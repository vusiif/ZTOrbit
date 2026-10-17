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
        QString error; auto lease=access->pluginServices().acquire("media",media::SessionService,media::SessionApi,&error);
        if(!lease) throw std::runtime_error(error.toStdString());
        devices->audio=std::make_shared<media::MediaSession>(std::move(lease));
        devices->zeroTier=&host.zeroTier();
        devices->files=&host.files();
        return {};
    }
    void requestStop(QObject *context,std::function<void(api::CreateResult)> completed) override {
        for(const auto &controller:controllers) if(controller) controller->stopForUnload();
        if(devices->audio) devices->audio->close(context,[completed](media::Result<bool> r) { completed({r.ok(),r.error.message}); });
        else completed({});
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
