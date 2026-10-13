#include <ztgui/PluginInterface.h>
#include <ztgui/PluginLifecycle.h>
#include "VoiceController.h"
#include <ztgui/PluginPage.h>
#include "AudioDeviceCache.h"
namespace vusiif::ztgui::voice_chat {
class VoiceChatPlugin final : public QObject, public vusiif::ztgui::api::PluginInterface,
    public vusiif::ztgui::api::PluginLifecycleInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ZTGUI_PLUGIN_IID FILE "plugin.json")
    Q_INTERFACES(vusiif::ztgui::api::PluginInterface vusiif::ztgui::api::PluginLifecycleInterface)
    std::shared_ptr<vusiif::ztgui::voice_chat::AudioDeviceCache> devices = std::make_shared<vusiif::ztgui::voice_chat::AudioDeviceCache>();
public:
    vusiif::ztgui::api::CreateTask onCreate(vusiif::ztgui::util::HostServices &,
                                       const vusiif::ztgui::util::CancellationToken &cancellation) override {
        return [cache = devices, cancellation] {
            const bool ready = cache->refresh([cancellation] { return cancellation.isCancellationRequested(); });
            return vusiif::ztgui::api::CreateResult{ready, ready ? QString() : QStringLiteral("设备初始化已取消")};
        };
    }
    vusiif::ztgui::api::PluginDefinition definition() override {
        vusiif::ztgui::api::PluginDefinition result;
        result.info = {"org.ztgui.voice-chat", QStringLiteral("语音聊天"), "0.1.0"};
        vusiif::ztgui::api::MenuContribution menu;
        menu.id = "rooms"; menu.title = QStringLiteral("语音聊天");
        menu.toolTip = QStringLiteral("连接语音服务器并加入房间");
        menu.createPage = [cache = devices](QQmlEngine *engine, QQuickItem *parent) {
            auto *controller = new VoiceController(nullptr, cache);
            auto *page = api::createQmlPage(engine, parent, QUrl("qrc:/ztgui/voice-chat/VoicePage.qml"),
                {{"controller", QVariant::fromValue(static_cast<QObject *>(controller))}});
            if (page) controller->setParent(page); else delete controller;
            return page;
        };
        result.menus.append(menu);
        return result;
    }
};
} // namespace vusiif::ztgui::voice_chat
#include "Plugin.moc"
