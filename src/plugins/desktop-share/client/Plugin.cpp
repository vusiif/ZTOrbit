#include <zto/PluginInterface.h>
#include <zto/PluginLifecycle.h>
#include <zto/PluginPage.h>
#include <zto/PluginServices.h>
#include <zto/PluginStop.h>
#include <ztmedia/MediaSession.h>
#include <ztmedia/VideoPreview.h>

#include "DeskShareController.h"
#include "Preferences.h"

#include <QDebug>
#include <QFile>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTimer>
#include <memory>

namespace vusiif::zto::desktop_share {

// The plugin owns one media session lease for its lifetime and hands it to the
// page controller. Opening the page only enumerates screen/window sources and
// reads the saved address/profile; the local capture, network connection and
// every room start exclusively from an explicit user action.
//
// The host caches one page per menu. Switching menus hides the page without
// destroying it, so an established session (connection, room, capture, viewer
// track) survives a menu switch and a hidden/minimized main window; the page
// still shows the live controller phase when it becomes visible again.
class DesktopSharePlugin final : public QObject,
                                 public vusiif::zto::api::PluginInterface,
                                 public vusiif::zto::api::PluginLifecycleInterface,
                                 public vusiif::zto::api::PluginStopInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ZTO_PLUGIN_IID FILE "plugin.json")
    Q_INTERFACES(vusiif::zto::api::PluginInterface vusiif::zto::api::PluginLifecycleInterface
                     vusiif::zto::api::PluginStopInterface)

public:
    vusiif::zto::api::CreateTask onCreate(
        vusiif::zto::util::HostServices &services,
        const vusiif::zto::util::CancellationToken &cancellation) override
    {
        m_services = &services;
        m_stopRequested = false;
        m_stopInProgress = false;
        m_stopSucceeded = false;
        m_stopPending.clear();
        m_preferences = std::make_unique<Preferences>(&services);
        // The lease must be acquired on the host event thread (onCreate runs
        // there); only cancellation checks and logging run in the background
        // task. Opening the page still does not enumerate or capture anything.
        auto *access = dynamic_cast<vusiif::zto::util::ServiceAccess *>(&services);
        QString failure;
        if (!access) {
            failure = QStringLiteral("宿主未提供插件服务接口");
        } else {
            QString error;
            auto lease = access->pluginServices().acquire(
                QStringLiteral("media"), vusiif::zto::media::SessionService,
                vusiif::zto::media::SessionApi, &error);
            if (!lease)
                failure = QStringLiteral("媒体服务不可用：%1")
                              .arg(error.isEmpty() ? QStringLiteral("未取得 media.session 租约")
                                                   : error);
            else
                m_session = std::make_shared<vusiif::zto::media::MediaSession>(lease);
        }
        return [this, failure, cancellation]() -> vusiif::zto::api::CreateResult {
            if (cancellation.isCancellationRequested())
                return {false, QStringLiteral("桌面共享插件初始化已取消")};
            if (!failure.isEmpty())
                return {false, failure};
            if (m_services)
                m_services->logs().write(
                    vusiif::zto::util::LogLevel::Info,
                    QStringLiteral("desktop-share 阶段6 初始化完成：媒体租约就绪，页面打开不联网不采集"));
            if (cancellation.isCancellationRequested())
                return {false, QStringLiteral("桌面共享插件初始化已取消")};
            return {true, QString()};
        };
    }

    // Idempotent, asynchronous stop. Repeated requests are queued instead of
    // starting a second teardown or failing the host's stop attempt. Completion
    // is reported only after the controller drained the network thread and every
    // media track removal; the plugin then closes the media lease, whose
    // completion waits for the worker to release its resources. A failure keeps
    // the DLL, provider leases and controllers alive so the host can retry.
    void requestStop(QObject *context,
                     std::function<void(vusiif::zto::api::CreateResult)> completed) override
    {
        Q_UNUSED(context);
        if (m_stopSucceeded) {
            QTimer::singleShot(0, this, [completed]() { completed({true, QString()}); });
            return;
        }
        m_stopPending.append(std::move(completed));
        if (m_stopRequested || m_stopInProgress) return;
        m_stopRequested = true;
        log(vusiif::zto::util::LogLevel::Info,
            QStringLiteral("desktop-share requestStop: begin"));
        QList<QPointer<DeskShareController>> controllers;
        for (const auto &controller : m_controllers) {
            if (controller) controllers.append(controller);
        }
        if (controllers.isEmpty()) {
            closeSession();
            return;
        }
        m_stopInProgress = true;
        struct Drain {
            int remaining = 0;
            bool ok = true;
        };
        auto drain = std::make_shared<Drain>();
        drain->remaining = controllers.size();
        for (const auto &controller : controllers) {
            controller->shutdownAsync([this, drain](bool ok) {
                drain->ok = drain->ok && ok;
                if (--drain->remaining) return;
                m_stopInProgress = false;
                log(vusiif::zto::util::LogLevel::Info,
                    QStringLiteral("desktop-share requestStop: controllers drained ok=%1")
                        .arg(drain->ok));
                if (!drain->ok) {
                    m_stopRequested = false;
                    completeStop(false, QStringLiteral(
                                            "停止未完成：网络或媒体在途回调尚未排空；保留 DLL、控制器和媒体租约"));
                    return;
                }
                closeSession();
            });
        }
    }

    void closeSession()
    {
        log(vusiif::zto::util::LogLevel::Info,
            QStringLiteral("desktop-share requestStop: closing media lease"));
        auto session = m_session;
        if (!session) {
            m_stopSucceeded = true;
            m_stopRequested = false;
            completeStop(true, QString());
            return;
        }
        session->close(this, [this, session](vusiif::zto::media::Result<bool> result) {
            log(result.ok() ? vusiif::zto::util::LogLevel::Info
                            : vusiif::zto::util::LogLevel::Warning,
                QStringLiteral("desktop-share requestStop: media lease closed ok=%1 %2")
                    .arg(result.ok()).arg(result.error.message));
            if (result.ok()) {
                m_controllers.clear();
                m_session.reset();
                m_stopSucceeded = true;
                m_stopRequested = false;
                completeStop(true, QString());
            } else {
                m_stopRequested = false;
                completeStop(false, result.error.message.isEmpty()
                                        ? QStringLiteral("媒体租约关闭失败，保留插件以便重试")
                                        : result.error.message);
            }
        });
    }

    void onDestroy() override
    {
        m_controllers.clear();
        m_session.reset();
        m_preferences.reset();
        m_services = nullptr;
        m_stopPending.clear();
    }

    vusiif::zto::api::PluginDefinition definition() override
    {
        using namespace vusiif::zto::api;
        PluginDefinition result;
        result.info = {QStringLiteral("desktop-share"), QStringLiteral("桌面共享"),
                       QStringLiteral("0.1.0")};
        MenuContribution menu;
        menu.id = QStringLiteral("share");
        menu.title = QStringLiteral("桌面共享");
        menu.toolTip = QStringLiteral("连接服务端，选择屏幕或窗口后主动共享，或输入房号观看");
        menu.createPage = [this](QQmlEngine *engine, QQuickItem *parent) {
            if (!m_session) return static_cast<QQuickItem *>(nullptr);
            auto *controller = new DeskShareController(m_session);
            controller->setPreferences(m_preferences.get());
            controller->initialize();
            auto *page = createQmlPage(
                engine, parent, QUrl(QStringLiteral("qrc:/zto/desktop-share/DeskSharePage.qml")),
                {{QStringLiteral("controller"),
                  QVariant::fromValue(static_cast<QObject *>(controller))}});
            if (!page) {
                QQmlComponent diagnostic(
                    engine,
                    QUrl(QStringLiteral("qrc:/zto/desktop-share/DeskSharePage.qml")));
                qWarning().noquote() << "Desktop-share page creation failed; resource"
                                     << QFile::exists(":/zto/desktop-share/DeskSharePage.qml")
                                     << "status" << diagnostic.status() << diagnostic.errorString();
                delete controller;
                return page;
            }
            controller->setParent(page);
            if (auto *host = page->findChild<QQuickItem *>(QStringLiteral("desktopSharePreview"))) {
                // The preview is a page-owned visual child created by the page
                // factory; no global QML type or singleton is registered.
                auto *preview = new vusiif::zto::media::VideoPreview(host);
                preview->setSize(host->size());
                QObject::connect(host, &QQuickItem::widthChanged, preview,
                                 [host, preview] { preview->setWidth(host->width()); });
                QObject::connect(host, &QQuickItem::heightChanged, preview,
                                 [host, preview] { preview->setHeight(host->height()); });
                controller->setPreview(preview);
            }
            m_controllers.append(controller);
            QObject::connect(page, &QObject::destroyed, this,
                             [this, controller] { m_controllers.removeAll(controller); });
            return page;
        };
        result.menus.append(menu);
        return result;
    }

private:
    void log(vusiif::zto::util::LogLevel level, const QString &message)
    {
        if (m_services) m_services->logs().write(level, message);
    }

    void completeStop(bool ok, const QString &message)
    {
        log(ok ? vusiif::zto::util::LogLevel::Info : vusiif::zto::util::LogLevel::Warning,
            QStringLiteral("desktop-share requestStop: completed ok=%1 %2").arg(ok).arg(message));
        const auto pending = std::move(m_stopPending);
        m_stopPending.clear();
        for (const auto &completed : pending) {
            if (completed) completed({ok, ok ? QString() : message});
        }
    }

    vusiif::zto::util::HostServices *m_services = nullptr;
    std::shared_ptr<vusiif::zto::media::MediaSession> m_session;
    std::unique_ptr<Preferences> m_preferences;
    QList<QPointer<DeskShareController>> m_controllers;
    bool m_stopRequested = false;
    bool m_stopInProgress = false;
    bool m_stopSucceeded = false;
    QList<std::function<void(vusiif::zto::api::CreateResult)>> m_stopPending;
};

} // namespace vusiif::zto::desktop_share

#include "Plugin.moc"
