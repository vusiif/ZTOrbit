#include <ztgui/PluginInterface.h>
#include <QQuickItem>
class SdkSample final : public QObject, public vusiif::ztgui::api::PluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ZTGUI_PLUGIN_IID FILE "sdk-plugin.json")
    Q_INTERFACES(vusiif::ztgui::api::PluginInterface)
public:
    vusiif::ztgui::api::PluginDefinition definition() override {
        using namespace vusiif::ztgui::api;
        MenuContribution menu; menu.id="page"; menu.title="SDK sample";
        menu.createPage=[](QQmlEngine *,QQuickItem *owner) { auto *page=new QQuickItem(owner); page->setParent(owner); return page; };
        return {{"SDK-sample","SDK sample","1"},{menu}};
    }
};
#include "SdkPlugin.moc"
