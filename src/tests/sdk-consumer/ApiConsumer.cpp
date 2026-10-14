#include <ztgui/PluginApi.h>
#include <ztgui/PluginInterface.h>
#include <ztgui/PluginLifecycle.h>
#include <ztgui/PluginFiles.h>
#include <ztgui/ZeroTierService.h>
#include <ztprotocol/NetworkAddress.h>
#include <type_traits>
#include <QQuickItem>
#include <QQmlEngine>

static_assert(vusiif::ztgui::api::ApiMajor == 0);
static_assert(vusiif::ztgui::api::ApiMinor == 4);
static_assert(vusiif::ztgui::api::isApiCompatible(0, 4));
static_assert(!vusiif::ztgui::api::isApiCompatible(0, 3));
static_assert(!vusiif::ztgui::api::isApiCompatible(0, 2));
static_assert(std::is_abstract_v<vusiif::ztgui::util::HostServices>);
static_assert(std::is_abstract_v<vusiif::ztgui::util::PluginFiles>);
static_assert(std::is_abstract_v<vusiif::ztgui::util::ZeroTierService>);

// These methods must compile using only installed public headers.
class ConsumerLifecycle : public vusiif::ztgui::api::PluginLifecycleInterface {
public:
    vusiif::ztgui::api::CreateTask onCreate(vusiif::ztgui::util::HostServices &services,
                                       const vusiif::ztgui::util::CancellationToken &cancellation) override {
        host = &services;
        return [this, cancellation] {
            if (cancellation.isCancellationRequested()) return vusiif::ztgui::api::CreateResult{false, "cancelled"};
            const auto saved = host->files().writeFile("state.txt", "ready");
            return vusiif::ztgui::api::CreateResult{saved.ok(), saved.error};
        };
    }
    void onEnteredFrom(const vusiif::ztgui::api::MenuLocation &) override {}
    void onExitedTo(const vusiif::ztgui::api::MenuLocation &) override {}
    void onDestroy() override { host = nullptr; }
private:
    vusiif::ztgui::util::HostServices *host = nullptr;
};

void consumeNetworkTools(vusiif::ztgui::util::ZeroTierService &service, QObject *context)
{
    service.sendDataTo("0123456789000000", "0123456789", QByteArray("binary\0data", 11), context,
        [](const vusiif::ztgui::util::SendResult &) {});
    service.broadcastDataTo("0123456789000000", {"0123456789"}, "broadcast", context,
        [](const vusiif::ztgui::util::SendResult &) {});
    service.broadcastDataBalancedTo("0123456789000000", {"0123456789"}, "data", context, [](const vusiif::ztgui::util::SendResult &) {});
    service.getNetworkRelays("0123456789000000", context, [](const vusiif::ztgui::util::RelayResult &) {});
    if(auto *sub=service.subscribeNetworkData("0123456789000000", context, [](const vusiif::ztgui::util::NetworkMessage &) {})) sub->cancel();

}

int main()
{
    const auto endpoint = vusiif::ztgui::protocol::serverEndpoint("example.com");
    if (!endpoint.valid() || endpoint.port != 18100) return 2;
    using namespace vusiif::ztgui::api;
    PluginDefinition plugin;
    plugin.info = {"example-sdk", "SDK consumer", "0.0.1"};
    MenuContribution menu;
    menu.id = "overview";
    menu.title = "SDK consumer";
    menu.createPage = [](QQmlEngine *, QQuickItem *parent) -> QQuickItem * { return new QQuickItem(parent); };
    plugin.menus.append(menu);
    return isApiCompatible(plugin.info.apiMajor, plugin.info.apiMinor) && plugin.menus.size() == 1 ? 0 : 1;
}
