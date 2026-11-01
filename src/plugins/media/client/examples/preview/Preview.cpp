#include <zto/PluginInterface.h>
#include <zto/PluginLifecycle.h>
#include <zto/PluginStop.h>
#include <zto/PluginPage.h>
#include <ztmedia/MediaSession.h>
#include <ztmedia/VideoPreview.h>
#include <QPointer>
#include <stdexcept>
using namespace vusiif::zto;
class PreviewController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList sources MEMBER sourceRows NOTIFY changed)
    Q_PROPERTY(QString status MEMBER status NOTIFY changed)
  public:
    QVariantList sourceRows;
    QString status = "选择来源后点击开始；枚举不会启动采集。";
    std::shared_ptr<media::MediaSession> session;
    QPointer<media::VideoPreview> raw, decoded;
    QString sender, receiver;
    quint64 epoch = 0;
    bool opening = false, opened = false;
    explicit PreviewController(std::shared_ptr<media::MediaSession> s) : session(std::move(s)) {
        session->error = [this](const media::MediaError &e) {
            status = e.message;
            emit changed();
        };
        session->packet = [this](media::EncodedPacket p) {
            if (p.track == sender && !receiver.isEmpty()) {
                p.track = receiver;
                if (!session->submit(std::move(p))) {
                    status = "数据通道积压";
                    emit changed();
                }
            }
        };
        session->frame = [this](const media::FrameView &f) {
            auto target = f.decoded ? decoded : raw;
            if (target)
                target->present(f);
        };
        session->sourcesChanged = [this] { refresh(); };
        refresh();
    }
    ~PreviewController() override {
        session->packet = {};
        session->frame = {};
        session->error = {};
        session->sourcesChanged = {};
    }
    Q_INVOKABLE void refresh() {
        session->sources(this, [this](media::Result<media::SourceSnapshot> r) {
            sourceRows.clear();
            for (const auto &s : r.value.sources)
                if (s.kind != "microphone" && s.kind != "speaker")
                    sourceRows.append(
                        QVariantMap{{"description", s.kind + " · " + s.description},
                                    {"source", s.kind == "synthetic" ? s.id : s.kind + ":" + s.id}});
            emit changed();
        });
    }
    Q_INVOKABLE void stop() {
        ++epoch;
        if (!sender.isEmpty())
            session->remove(sender, this);
        if (!receiver.isEmpty())
            session->remove(receiver, this);
        sender.clear();
        receiver.clear();
        if (raw)
            raw->clear();
        if (decoded)
            decoded->clear();
        status = "已停止";
        emit changed();
    }
    Q_INVOKABLE void start(const QString &source) {
        if (opening)
            return;
        stop();
        auto revision = epoch;
        auto build = [this, source, revision] {
            media::TrackConfig c;
            c.kind = "video";
            c.source = source;
            c.preview = true;
            c.codec.codec = "h264";
            c.codec.bitrate = 2000000;
            c.codec.allowAdjustment = true;
            session->createTrack(
                c, this, [this, c, revision](media::Result<media::TrackDescriptor> r) mutable {
                    if (!r.ok()) {
                        status = r.error.message;
                        emit changed();
                        return;
                    }
                    if (epoch != revision) {
                        session->remove(r.value.id, this);
                        return;
                    }
                    sender = r.value.id;
                    c.direction = "receive";
                    c.source.clear();
                    session->createTrack(c, this, [this, revision](media::Result<media::TrackDescriptor> r) {
                        if (!r.ok()) {
                            status = r.error.message;
                            emit changed();
                            return;
                        }
                        if (epoch != revision) {
                            session->remove(r.value.id, this);
                            return;
                        }
                        receiver = r.value.id;
                        session->start(receiver, this, [this, revision](media::Result<bool> r) {
                            if (epoch != revision)
                                return;
                            if (!r.ok()) {
                                status = r.error.message;
                                emit changed();
                                return;
                            }
                            session->start(sender, this, [this, revision](media::Result<bool> r) {
                                if (epoch != revision)
                                    return;
                                status = r.ok() ? QString("正在回送 · 1280×720 · 30 fps · 2 Mbps")
                                                : r.error.message;
                                emit changed();
                            });
                        });
                    });
                });
        };
        if (opened) {
            build();
            return;
        }
        opening = true;
        session->open(this, [this, build, revision](media::Result<bool> r) {
            opening = false;
            if (!r.ok()) {
                status = r.error.message;
                emit changed();
                return;
            }
            opened = true;
            if (epoch == revision)
                build();
        });
    }
  signals:
    void changed();
};
class PreviewPlugin final : public QObject,
                            public api::PluginInterface,
                            public api::PluginLifecycleInterface,
                            public api::PluginStopInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ZTO_PLUGIN_IID FILE "plugin.json")
    Q_INTERFACES(vusiif::zto::api::PluginInterface vusiif::zto::api::PluginLifecycleInterface
                     vusiif::zto::api::PluginStopInterface)
    std::shared_ptr<media::MediaSession> session;
    QPointer<PreviewController> controller;

  public:
    api::CreateTask onCreate(util::HostServices &host, const util::CancellationToken &) override {
        auto *access = dynamic_cast<util::ServiceAccess *>(&host);
        if (!access)
            throw std::runtime_error("Host service access unavailable");
        QString error;
        auto lease =
            access->pluginServices().acquire("media", media::SessionService, media::SessionApi, &error);
        if (!lease)
            throw std::runtime_error(error.toStdString());
        session = std::make_shared<media::MediaSession>(lease);
        return {};
    }
    void requestStop(QObject *context, std::function<void(api::CreateResult)> completed) override {
        if (controller)
            controller->stop();
        session->close(context, [completed](media::Result<bool> r) { completed({r.ok(), r.error.message}); });
    }
    api::PluginDefinition definition() override {
        api::PluginDefinition d;
        d.info = {"media-preview-probe", QStringLiteral("媒体开发预览"), "0.1.0"};
        api::MenuContribution menu;
        menu.id = "preview";
        menu.title = QStringLiteral("媒体开发预览");
        menu.createPage = [this](QQmlEngine *engine, QQuickItem *parent) {
            auto *c = new PreviewController(session);
            controller = c;
            auto *page = api::createQmlPage(engine, parent, QUrl("qrc:/ztmedia/probe/Preview.qml"),
                                            {{"controller", QVariant::fromValue(static_cast<QObject *>(c))}});
            if (!page) {
                delete c;
                return page;
            }
            c->setParent(page);
            auto create = [page](const char *name) {
                auto *host = page->findChild<QQuickItem *>(name);
                if (!host)
                    return static_cast<media::VideoPreview *>(nullptr);
                auto *p = new media::VideoPreview(host);
                p->setSize(host->size());
                QObject::connect(host, &QQuickItem::widthChanged, p,
                                 [host, p] { p->setWidth(host->width()); });
                QObject::connect(host, &QQuickItem::heightChanged, p,
                                 [host, p] { p->setHeight(host->height()); });
                return p;
            };
            c->raw = create("sourcePreviewHost");
            c->decoded = create("decodedPreviewHost");
            return page;
        };
        d.menus.append(menu);
        return d;
    }
};
#include "Preview.moc"
