#include <ztgui/PluginInterface.h>
#include <ztgui/PluginLifecycle.h>
#include <QThread>
#include <QFile>
#include <QLabel>
#include <QLockFile>
#include <memory>
static void mark(const char *text) {
    if (qEnvironmentVariableIsEmpty("ZTGUI_PLUGIN_LIFETIME_FILE")) return;
    const auto path = qEnvironmentVariable("ZTGUI_PLUGIN_LIFETIME_FILE");
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(2000)) return;
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) file.write(QByteArray(text) + '\n');
}
static void preparationStarted() {
    const auto started = qEnvironmentVariable("ZTGUI_PLUGIN_PREPARATION_STARTED_FILE");
    if (!started.isEmpty()) { QFile file(started); if (file.open(QIODevice::WriteOnly)) file.write("started"); }
}
struct Resource { ~Resource() { mark("factory"); } };
class Page : public QLabel {
public:
    explicit Page(QWidget *parent) : QLabel("dynamic fixture", parent) {}
    ~Page() override { mark("page"); }
};
class Fixture final : public QObject, public vusiif::ztgui::api::PluginInterface
#if defined(FIXTURE_LIFECYCLE) || defined(FIXTURE_CANCELLABLE)
    , public vusiif::ztgui::api::PluginLifecycleInterface
#endif
{
    Q_OBJECT
#ifdef FIXTURE_OBSOLETE_IID
    Q_PLUGIN_METADATA(IID "org.ztgui.PluginInterface/1.0" FILE "fixture.json")
#else
    Q_PLUGIN_METADATA(IID ZTGUI_PLUGIN_IID FILE "fixture.json")
#endif
    Q_INTERFACES(vusiif::ztgui::api::PluginInterface)
#if defined(FIXTURE_LIFECYCLE) || defined(FIXTURE_CANCELLABLE)
    Q_INTERFACES(vusiif::ztgui::api::PluginLifecycleInterface)
#endif
public:
#ifdef FIXTURE_CANCELLABLE
    vusiif::ztgui::api::CreateTask onCreate(vusiif::ztgui::util::HostServices &,
                                       const vusiif::ztgui::util::CancellationToken &cancellation) override {
        mark("create");
        return [cancellation] {
            const auto id = QStringLiteral(FIXTURE_ID);
            mark(("started:" + id).toUtf8().constData());
#if FIXTURE_CANCELLABLE == 2
            QThread::msleep(350); // Models a third-party call without cancellation.
#else
            const int steps = qEnvironmentVariableIntValue("ZTGUI_PLUGIN_CREATE_STEPS");
            for (int i = 0; i < (steps > 0 ? steps : 20); ++i) {
                if (cancellation.isCancellationRequested()) break;
                QThread::msleep(10);
            }
#endif
            if (cancellation.isCancellationRequested()) {
                mark(("cancelled:" + id).toUtf8().constData());
                return vusiif::ztgui::api::CreateResult{false, "cancelled"};
            }
            mark(("prepared:" + id).toUtf8().constData());
            return vusiif::ztgui::api::CreateResult{};
        };
    }
#endif
#ifdef FIXTURE_LIFECYCLE
    vusiif::ztgui::api::CreateTask onCreate(vusiif::ztgui::util::HostServices &services,
                                          const vusiif::ztgui::util::CancellationToken &) override {
        mark("create");
        services.files().readFile("fixture/missing.txt");
        return [] {
            preparationStarted();
            QThread::msleep(150);
            mark("prepared");
            return vusiif::ztgui::api::CreateResult{FIXTURE_LIFECYCLE == 1, "fixture create failed"};
        };
    }
#endif
#if defined(FIXTURE_LIFECYCLE) || defined(FIXTURE_CANCELLABLE)
    void onDestroy() override { mark("destroy"); }
    void onEnteredFrom(const vusiif::ztgui::api::MenuLocation &previous) override {
        mark(("entered:" + previous.menu.id).toUtf8().constData());
    }
    void onExitedTo(const vusiif::ztgui::api::MenuLocation &next) override {
        mark(("exited:" + next.menu.id).toUtf8().constData());
    }
#endif
    ~Fixture() override { mark("entry"); }
    vusiif::ztgui::api::PluginDefinition definition() override {
        auto resource = std::make_shared<Resource>();
        vusiif::ztgui::api::PluginDefinition result;
        result.info = {"test.dynamic", "Dynamic", "1"};
#ifdef FIXTURE_LIFECYCLE
        result.info.id = "test.lifecycle";
#endif
#ifdef FIXTURE_CANCELLABLE
        result.info.id = QStringLiteral(FIXTURE_ID);
        result.info.name = QStringLiteral(FIXTURE_ID);
#endif
        vusiif::ztgui::api::MenuContribution menu;
        menu.id = "overview"; menu.title = "Dynamic";
        menu.createPage = [resource](QWidget *parent) -> QWidget * { return new Page(parent); };
        result.menus.append(menu); return result;
    }
};
#include "Fixture.moc"
