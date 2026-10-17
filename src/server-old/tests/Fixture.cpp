#include <ztserver/PluginInterface.h>
#include <QThread>
using namespace vusiif::ztgui;
class Fixture final : public QObject, public server::api::PluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ZTSERVER_PLUGIN_IID FILE "fixture.json")
    Q_INTERFACES(vusiif::ztgui::server::api::PluginInterface)
    server::api::HostServices *host = nullptr;
public:
    server::api::PluginInfo info() const override { return {FIXTURE_ID,FIXTURE_ID,"1"}; }
    server::api::StartTask onStart(server::api::HostServices &services, const util::CancellationToken &cancellation) override {
        host = &services;
        host->files().writeFile("started","yes");
        host->logs().write(util::LogLevel::Info,"fixture-start");
#ifdef FIXTURE_CANCELLABLE
        const auto token = cancellation;
        struct Captured {
            server::api::HostServices *host;
            ~Captured() { host->files().writeFile("capture-destroyed","yes"); }
        };
        auto captured = std::make_shared<Captured>(); captured->host = host;
        return [this, token, captured] {
            host->files().writeFile("initialization-started","yes");
            while (!token.isCancellationRequested()) QThread::msleep(10);
            host->files().writeFile("cancelled","yes");
            return server::api::StartResult{false,"cancelled"};
        };
#elif defined(FIXTURE_FAILED)
        return [] { return server::api::StartResult{false,"fixture-failed"}; };
#else
        return {};
#endif
    }
    void onTcpPacket(server::api::ConnectionId connection, const QByteArray &data) override { host->sendTcp(connection,data); }
    void onUdpPacket(const server::api::Endpoint &endpoint, const QByteArray &data) override { host->sendUdp(endpoint,data); }
    void onConnectionClosed(server::api::ConnectionId) override { host->files().writeFile("closed","yes"); }
    void onStop() override { if (host) host->files().writeFile("stopped","yes"); }
};
#include "Fixture.moc"
