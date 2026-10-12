#include <ztgui/PluginApi.h>
#include <ztgui/PluginInterface.h>
#include <ztgui/PluginLifecycle.h>
#include <ztgui/PluginFiles.h>
#include <ztgui/ZeroTierService.h>
#include <type_traits>
#include <QLabel>

static_assert(vusiif::ztgui::api::ApiMajor == 0);
static_assert(vusiif::ztgui::api::ApiMinor == 3);
static_assert(vusiif::ztgui::api::isApiCompatible(0, 3));
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

int main()
{
    using namespace vusiif::ztgui::api;
    PluginDefinition plugin;
    plugin.info = {"example.sdk", "SDK consumer", "0.0.1"};
    MenuContribution menu;
    menu.id = "overview";
    menu.title = "SDK consumer";
    menu.createPage = [](QWidget *parent) -> QWidget * { return new QLabel("SDK page", parent); };
    plugin.menus.append(menu);
    return isApiCompatible(plugin.info.apiMajor, plugin.info.apiMinor) && plugin.menus.size() == 1 ? 0 : 1;
}
