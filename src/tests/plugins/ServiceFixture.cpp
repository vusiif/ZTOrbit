#include <ztgui/PluginInterface.h>
#include <ztgui/PluginLifecycle.h>
#include <ztgui/PluginStop.h>
#include <ztgui/PluginServices.h>
#include <QQuickItem>
#include <QTimer>
#include <QFile>
#include <QCoreApplication>
#include <QEventLoop>
#include <stdexcept>
using namespace vusiif::ztgui;
static void trace(const QByteArray &value) { const auto path=qEnvironmentVariable("ZTGUI_SERVICE_TRACE"); if(path.isEmpty()) return; QFile f(path); if(f.open(QIODevice::WriteOnly|QIODevice::Append)) { f.write(value); f.write("\n"); } }
class ServiceFixture final : public QObject,public api::PluginInterface,public api::PluginLifecycleInterface,public api::PluginStopInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ZTGUI_PLUGIN_IID FILE "service-fixture.json")
    Q_INTERFACES(vusiif::ztgui::api::PluginInterface vusiif::ztgui::api::PluginLifecycleInterface vusiif::ztgui::api::PluginStopInterface)
    std::shared_ptr<util::ServiceLease> lease;
    bool rejectionEntered=false;
    void flood(bool closeNow) {
        for(int i=0;i<192;++i) lease->call("echo",{}, {},QCoreApplication::instance(),[this](const util::ServiceResult &r) {
            if(r.ok() || rejectionEntered) return;
            rejectionEntered=true; trace("rejection-start:" FIXTURE_SERVICE_ID);
            QEventLoop nested; QTimer::singleShot(300,&nested,&QEventLoop::quit); nested.exec();
            trace("rejection-end:" FIXTURE_SERVICE_ID);
        });
        if(closeNow) lease->close(nullptr,{});
    }
public:
    ServiceFixture() { trace("entry:" FIXTURE_SERVICE_ID); }
    ~ServiceFixture() override { trace("destroy:" FIXTURE_SERVICE_ID); }
    api::PluginDefinition definition() override {
        api::PluginDefinition d; d.info={FIXTURE_SERVICE_ID,FIXTURE_SERVICE_ID,"1.0.0"};
        api::MenuContribution m; m.id="test"; m.title="Test"; m.createPage=[this](QQmlEngine *,QQuickItem *p) {
            auto *item=new QQuickItem(p); item->setParent(p); item->setProperty("fixture",QVariant::fromValue(static_cast<QObject *>(this))); return item;
        }; d.menus.append(m); return d;
    }
    api::CreateTask onCreate(util::HostServices &host,const util::CancellationToken &) override {
        auto *access=dynamic_cast<util::ServiceAccess *>(&host); if(!access) throw std::runtime_error("No services");
        QString error; lease=access->pluginServices().acquire("fixture-provider",FIXTURE_SERVICE_NAME,"1",&error);
        if(!lease) throw std::runtime_error(error.toStdString());
        ping();
        if(qEnvironmentVariable("ZTGUI_SERVICE_INIT_FAIL")==FIXTURE_SERVICE_ID) return [] { return api::CreateResult{false,"fixture init failed"}; };
        return {};
    }
    Q_INVOKABLE void ping() {
        lease->call("echo",{{"consumer",FIXTURE_SERVICE_ID}},QByteArray("binary\0audio",12),this,[this](const util::ServiceResult &r) {
            if(r.ok() && r.payload==QByteArray("binary\0audio",12)) { setProperty("replies",property("replies").toInt()+1); trace("echo:" FIXTURE_SERVICE_ID); }
        });
    }
    Q_INVOKABLE void crashProvider() { lease->call("crash",{}, {},this,[](const util::ServiceResult &) {}); }
    Q_INVOKABLE void nestedRejections() { flood(false); }
    Q_INVOKABLE void canceledRejections() { flood(true); }
    void requestStop(QObject *context,std::function<void(api::CreateResult)> completed) override {
        trace("stop:" FIXTURE_SERVICE_ID);
        if(qEnvironmentVariable("ZTGUI_SERVICE_STOP_FAIL")==FIXTURE_SERVICE_ID) { completed({false,"fixture stop failed"}); return; }
        // Exercise a genuinely asynchronous stop while pages and DLL are retained.
        QTimer::singleShot(80,context,[this,context,completed] {
            lease->close(context,[completed](const util::ServiceResult &r) { completed({r.ok(),r.error}); });
        });
    }
};
#include "ServiceFixture.moc"
