#include <zto/PluginApi.h>
#include <zto/PluginInterface.h>
#include <zto/PluginLifecycle.h>
#include <zto/PluginFiles.h>
#include <zto/PluginStorage.h>
#include <zto/ZeroTierService.h>
#include <ztprotocol/NetworkAddress.h>
#include <type_traits>
#include <QQuickItem>
#include <QQmlEngine>

static_assert(vusiif::zto::api::ApiMajor == 0);
static_assert(vusiif::zto::api::ApiMinor == 5);
static_assert(vusiif::zto::api::isApiCompatible(0, 5));
static_assert(!vusiif::zto::api::isApiCompatible(0, 4));
static_assert(!vusiif::zto::api::isApiCompatible(0, 2));
static_assert(std::is_abstract_v<vusiif::zto::util::HostServices>);
static_assert(std::is_abstract_v<vusiif::zto::util::PluginFiles>);
static_assert(std::is_abstract_v<vusiif::zto::util::PluginStorage>);
static_assert(std::is_abstract_v<vusiif::zto::util::ZeroTierService>);

// The installed SDK exposes the optional cross-process transaction extension
// only through a dynamic_cast on the borrowed file service.
void consumeStorage(vusiif::zto::util::HostServices &services)
{
    auto *storage = dynamic_cast<vusiif::zto::util::PluginStorage *>(&services.files());
    if (!storage) return;
    const auto transaction = storage->updateFile("state.txt",
        [](const vusiif::zto::util::PluginFileSnapshot &snapshot) {
            if (!snapshot.exists) return vusiif::zto::util::PluginFileUpdate::keep();
            return vusiif::zto::util::PluginFileUpdate::replace(snapshot.data);
        });
    if (transaction.ok && transaction.changed) {
        const auto allowed = storage->legacyImportAllowed();
        if (allowed.ok() && allowed.allowed) (void)storage->completeLegacyImport();
    }
}

// These methods must compile using only installed public headers.
class ConsumerLifecycle : public vusiif::zto::api::PluginLifecycleInterface {
public:
    vusiif::zto::api::CreateTask onCreate(vusiif::zto::util::HostServices &services,
                                       const vusiif::zto::util::CancellationToken &cancellation) override {
        host = &services;
        return [this, cancellation] {
            if (cancellation.isCancellationRequested()) return vusiif::zto::api::CreateResult{false, "cancelled"};
            const auto saved = host->files().writeFile("state.txt", "ready");
            return vusiif::zto::api::CreateResult{saved.ok(), saved.error};
        };
    }
    void onEnteredFrom(const vusiif::zto::api::MenuLocation &) override {}
    void onExitedTo(const vusiif::zto::api::MenuLocation &) override {}
    void onDestroy() override { host = nullptr; }
private:
    vusiif::zto::util::HostServices *host = nullptr;
};

void consumeNetworkTools(vusiif::zto::util::ZeroTierService &service, QObject *context)
{
    service.sendDataTo("0123456789000000", "0123456789", QByteArray("binary\0data", 11), context,
        [](const vusiif::zto::util::SendResult &) {});
    service.broadcastDataTo("0123456789000000", {"0123456789"}, "broadcast", context,
        [](const vusiif::zto::util::SendResult &) {});
    service.broadcastDataBalancedTo("0123456789000000", {"0123456789"}, "data", context, [](const vusiif::zto::util::SendResult &) {});
    service.getNetworkRelays("0123456789000000", context, [](const vusiif::zto::util::RelayResult &) {});
    if(auto *sub=service.subscribeNetworkData("0123456789000000", context, [](const vusiif::zto::util::NetworkMessage &) {})) sub->cancel();

}

int main()
{
    const auto endpoint = vusiif::zto::protocol::serverEndpoint("example.com");
    if (!endpoint.valid() || endpoint.port != 18100) return 2;
    using namespace vusiif::zto::api;
    PluginDefinition plugin;
    plugin.info = {"example-sdk", "SDK consumer", "0.0.1"};
    MenuContribution menu;
    menu.id = "overview";
    menu.title = "SDK consumer";
    menu.createPage = [](QQmlEngine *, QQuickItem *parent) -> QQuickItem * { return new QQuickItem(parent); };
    plugin.menus.append(menu);
    return isApiCompatible(plugin.info.apiMajor, plugin.info.apiMinor) && plugin.menus.size() == 1 ? 0 : 1;
}
