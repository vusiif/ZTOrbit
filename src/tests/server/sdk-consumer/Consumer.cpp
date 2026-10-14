#include <ztserver/PluginInterface.h>
#include <ztprotocol/WireProtocol.h>
#include <type_traits>
static_assert(std::is_abstract_v<vusiif::ztgui::server::api::HostServices>);
void consumeNetworkTools(vusiif::ztgui::server::api::HostServices &host, QObject *context) {
    host.sendDataTo("0123456789000000", "0123456789", "data", context,
        [](const vusiif::ztgui::util::SendResult &) {});
    host.broadcastDataTo("0123456789000000", {"0123456789"}, "data", context,
        [](const vusiif::ztgui::util::SendResult &) {});
    host.broadcastDataBalancedTo("0123456789000000", {"0123456789"}, "data", context, [](const vusiif::ztgui::util::SendResult &) {});
    host.getNetworkRelays("0123456789000000", context, [](const vusiif::ztgui::util::RelayResult &) {});
    if(auto *sub=host.subscribeNetworkData("0123456789000000", context, [](const vusiif::ztgui::util::NetworkMessage &) {})) sub->cancel();

}
int main() {
    using namespace vusiif::ztgui;
    QByteArray bytes=protocol::frame("third-party","sdk"); protocol::Packet packet;
    return protocol::takeFrame(bytes,packet)==protocol::ReadResult::Packet && packet.payload=="sdk" ? 0 : 1;
}
