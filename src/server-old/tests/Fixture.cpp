#include <ztohub/PluginInterface.h>
#include <QThread>
using namespace vusiif::zto;
class Fixture final : public QObject, public vusiif::ztohub::api::PluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ZTOHUB_PLUGIN_IID FILE "fixture.json")
    Q_INTERFACES(vusiif::ztohub::api::PluginInterface)
    vusiif::ztohub::api::HostServices *host = nullptr;
public:
    vusiif::ztohub::api::PluginInfo info() const override { return {FIXTURE_ID,FIXTURE_ID,"1"}; }
    vusiif::ztohub::api::StartTask onStart(vusiif::ztohub::api::HostServices &services, const vusiif::zto::util::CancellationToken &cancellation) override {
        host = &services;
        host->files().writeFile("started","yes");
        host->logs().write(vusiif::zto::util::LogLevel::Info,"fixture-start");
#ifdef FIXTURE_CANCELLABLE
        const auto token = cancellation;
        struct Captured {
            vusiif::ztohub::api::HostServices *host;
            ~Captured() { host->files().writeFile("capture-destroyed","yes"); }
        };
        auto captured = std::make_shared<Captured>(); captured->host = host;
        return [this, token, captured] {
            host->files().writeFile("initialization-started","yes");
            while (!token.isCancellationRequested()) QThread::msleep(10);
            host->files().writeFile("cancelled","yes");
            return vusiif::ztohub::api::StartResult{false,"cancelled"};
        };
#elif defined(FIXTURE_FAILED)
        return [] { return vusiif::ztohub::api::StartResult{false,"fixture-failed"}; };
#else
        return {};
#endif
    }
    void onTcpPacket(vusiif::ztohub::api::ConnectionId connection, const QByteArray &data) override { host->sendTcp(connection,data); }
    void onUdpPacket(const vusiif::ztohub::api::Endpoint &endpoint, const QByteArray &data) override { host->sendUdp(endpoint,data); }
    void onConnectionClosed(vusiif::ztohub::api::ConnectionId) override { host->files().writeFile("closed","yes"); }
    void onStop() override { if (host) host->files().writeFile("stopped","yes"); }
};
#include "Fixture.moc"
