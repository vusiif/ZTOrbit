#include <zto/PluginInterface.h>
#include <zto/PluginLifecycle.h>
#include <QThread>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <stdexcept>
#include <QFile>
#include <QQuickItem>
#include <QQmlEngine>
#include <QLockFile>
#include <memory>
static void mark(const char *text) {
    if (qEnvironmentVariableIsEmpty("ZTO_PLUGIN_LIFETIME_FILE")) return;
    const auto path = qEnvironmentVariable("ZTO_PLUGIN_LIFETIME_FILE");
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(2000)) return;
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) file.write(QByteArray(text) + '\n');
}
static void preparationStarted() {
    const auto started = qEnvironmentVariable("ZTO_PLUGIN_PREPARATION_STARTED_FILE");
    if (!started.isEmpty()) { QFile file(started); if (file.open(QIODevice::WriteOnly)) file.write("started"); }
}
struct Resource { ~Resource() { mark("factory"); } };
class Page : public QQuickItem {
public:
    explicit Page(QQuickItem *parent) : QQuickItem(parent) { setParent(parent); }
    ~Page() override { mark("page"); }
};
class Fixture final : public QObject, public vusiif::zto::api::PluginInterface
#if defined(FIXTURE_LIFECYCLE) || defined(FIXTURE_CANCELLABLE)
    , public vusiif::zto::api::PluginLifecycleInterface
#endif
{
    Q_OBJECT
#ifdef FIXTURE_OBSOLETE_IID
    Q_PLUGIN_METADATA(IID "vusiif.zto.api.PluginInterface/1.0" FILE "fixture.json")
#else
    Q_PLUGIN_METADATA(IID ZTO_PLUGIN_IID FILE "fixture.json")
#endif
    Q_INTERFACES(vusiif::zto::api::PluginInterface)
#if defined(FIXTURE_LIFECYCLE) || defined(FIXTURE_CANCELLABLE)
    Q_INTERFACES(vusiif::zto::api::PluginLifecycleInterface)
#endif
    vusiif::zto::util::HostServices *borrowed=nullptr;
public:
#ifdef FIXTURE_CANCELLABLE
    vusiif::zto::api::CreateTask onCreate(vusiif::zto::util::HostServices &,
                                       const vusiif::zto::util::CancellationToken &cancellation) override {
        mark("create");
        return [cancellation] {
            const auto id = QStringLiteral(FIXTURE_ID);
            mark(("started:" + id).toUtf8().constData());
            preparationStarted();
#if FIXTURE_CANCELLABLE == 2
            QThread::msleep(350); // Models a third-party call without cancellation.
#else
            const int steps = qEnvironmentVariableIntValue("ZTO_PLUGIN_CREATE_STEPS");
            for (int i = 0; i < (steps > 0 ? steps : 20); ++i) {
                if (cancellation.isCancellationRequested()) break;
                QThread::msleep(10);
            }
#endif
            if (cancellation.isCancellationRequested()) {
                mark(("cancelled:" + id).toUtf8().constData());
                return vusiif::zto::api::CreateResult{false, "cancelled"};
            }
            mark(("prepared:" + id).toUtf8().constData());
            return vusiif::zto::api::CreateResult{};
        };
    }
#endif
#ifdef FIXTURE_LIFECYCLE
    vusiif::zto::api::CreateTask onCreate(vusiif::zto::util::HostServices &services,
                                          const vusiif::zto::util::CancellationToken &) override {
        mark("create"); borrowed=&services;
        services.files().readFile("fixture/missing.txt");
#ifdef FIXTURE_LEGACY_API
        services.zeroTier().getZTInfo(this, [](const vusiif::zto::util::JsonResult &) { mark("legacy-query"); });
#endif
        return [] {
            preparationStarted();
            QThread::msleep(150);
            mark("prepared");
            return vusiif::zto::api::CreateResult{FIXTURE_LIFECYCLE == 1, "fixture create failed"};
        };
    }
#endif
#if defined(FIXTURE_LIFECYCLE) || defined(FIXTURE_CANCELLABLE)
    void onDestroy() override { mark("destroy"); if(qEnvironmentVariableIsSet("ZTO_PLUGIN_DESTROY_THROWS")) throw std::runtime_error("fixture cleanup error"); }
    void onEnteredFrom(const vusiif::zto::api::MenuLocation &previous) override {
        mark(("entered:" + previous.menu.id).toUtf8().constData());
        if(borrowed && qEnvironmentVariableIsSet("ZTO_PLUGIN_NESTED_CALLBACK"))
            borrowed->zeroTier().getNetworkRelays("0123456789abcdef",this,[](const vusiif::zto::util::RelayResult &) {
                mark("callback-start"); QElapsedTimer elapsed; elapsed.start();
                while(elapsed.elapsed()<250) { QCoreApplication::processEvents(); QThread::msleep(1); }
                mark("callback-end");
            });
    }
    void onExitedTo(const vusiif::zto::api::MenuLocation &next) override {
        mark(("exited:" + next.menu.id).toUtf8().constData());
    }
#endif
    ~Fixture() override { mark("entry"); }
    vusiif::zto::api::PluginDefinition definition() override {
        auto resource = std::make_shared<Resource>();
        vusiif::zto::api::PluginDefinition result;
        result.info = {"test-dynamic", "Dynamic", "1"};
#ifdef FIXTURE_LIFECYCLE
        result.info.id = "test-lifecycle";
#endif
#ifdef FIXTURE_CANCELLABLE
        result.info.id = QStringLiteral(FIXTURE_ID);
        result.info.name = QStringLiteral(FIXTURE_ID);
#endif
        vusiif::zto::api::MenuContribution menu;
        menu.id = "overview"; menu.title = "Dynamic";
        menu.createPage = [resource](QQmlEngine *, QQuickItem *parent) -> QQuickItem * { return new Page(parent); };
        result.menus.append(menu); return result;
    }
};
#include "Fixture.moc"
