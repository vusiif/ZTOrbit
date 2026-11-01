#include <ztohub/PluginInterface.h>
#include <ztprotocol/WireProtocol.h>
#include <type_traits>
static_assert(std::is_abstract_v<vusiif::ztohub::api::HostServices>);
void consumeNetworkTools(vusiif::ztohub::api::HostServices &host, QObject *context) {
    host.sendDataTo("0123456789000000", "0123456789", "data", context,
        [](const vusiif::zto::util::SendResult &) {});
    host.broadcastDataTo("0123456789000000", {"0123456789"}, "data", context,
        [](const vusiif::zto::util::SendResult &) {});
    host.broadcastDataBalancedTo("0123456789000000", {"0123456789"}, "data", context, [](const vusiif::zto::util::SendResult &) {});
    host.getNetworkRelays("0123456789000000", context, [](const vusiif::zto::util::RelayResult &) {});
    if(auto *sub=host.subscribeNetworkData("0123456789000000", context, [](const vusiif::zto::util::NetworkMessage &) {})) sub->cancel();

}
int main() {
    using namespace vusiif::zto;
    QByteArray bytes=vusiif::zto::protocol::frame("third-party","sdk"); vusiif::zto::protocol::Packet packet;
    return vusiif::zto::protocol::takeFrame(bytes,packet)==vusiif::zto::protocol::ReadResult::Packet && packet.payload=="sdk" ? 0 : 1;
}
