#include <ztgui/PluginApi.h>
#include <QLabel>

static_assert(ZtGui::Plugins::ApiMajor == 0);
static_assert(ZtGui::Plugins::ApiMinor == 2);
static_assert(ZtGui::Plugins::isApiCompatible(0, 2));
static_assert(!ZtGui::Plugins::isApiCompatible(0, 1));

int main()
{
    using namespace ZtGui::Plugins;
    PluginDefinition plugin;
    plugin.info = {"example.sdk", "SDK consumer", "0.0.1"};
    MenuContribution menu;
    menu.id = "overview";
    menu.title = "SDK consumer";
    menu.createPage = [](QWidget *parent) -> QWidget * { return new QLabel("SDK page", parent); };
    plugin.menus.append(menu);
    return isApiCompatible(plugin.info.apiMajor, plugin.info.apiMinor) && plugin.menus.size() == 1 ? 0 : 1;
}
