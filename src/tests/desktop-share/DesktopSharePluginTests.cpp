#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "../../client/main/plugins/PluginLoader.h"
#include "../../client/main/plugins/PluginRegistry.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPainter>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSet>
#include <QSettings>
#include <QScreen>
#include <QStyle>
#include <QCryptographicHash>
#include <QTcpServer>
#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QWidget>
#include <QUuid>
#include <QtTest>
#include <QAbstractEventDispatcher>
#include <QThread>
#include <atomic>

using namespace vusiif::zto;
using namespace vusiif::zto::plugins;

namespace {
// Loads the independently built desktop-share package with the real host
// PluginLoader/PluginRegistry and keeps the asynchronous unload asynchronous.
class Fixture {
public:
    Fixture()
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        if (settings.isValid())
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        // Host the page in a real window so the preview is rendered by the
        // configured Qt Quick backend (software by default, GPU on request).
        window.setColor(Qt::black);
        window.resize(960, 640);
        parent.setParentItem(window.contentItem());
        parent.setSize(QSizeF(window.width(), window.height()));
        QObject::connect(&window, &QQuickWindow::widthChanged, &parent,
                         [this] { parent.setWidth(window.width()); });
        QObject::connect(&window, &QQuickWindow::heightChanged, &parent,
                         [this] { parent.setHeight(window.height()); });
    }
    ~Fixture()
    {
        if (loaded) {
            QString ignored;
            unload(&ignored);
        }
        loader.requestShutdown();
        QElapsedTimer timer;
        timer.start();
        while (!loader.shutdownComplete() && timer.elapsed() < 20000) QTest::qWait(25);
    }
    bool load(QString *error)
    {
        if (!install.isValid()) {
            *error = QStringLiteral("cannot create install directory");
            return false;
        }
        if (!QFile::copy(DESKTOP_SHARE_PLUGIN, install.filePath(QStringLiteral("desktop-share.z2plugin")))) {
            *error = QStringLiteral("copy desktop-share package");
            return false;
        }
        if (!QFile::copy(MEDIA_PLUGIN, install.filePath(QStringLiteral("media.z2plugin")))) {
            *error = QStringLiteral("copy media package");
            return false;
        }
        loader.scan(install.path(), registry);
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 80000) {
            bool registered = false;
            for (const auto &info : registry.plugins())
                if (info.id == QStringLiteral("desktop-share")) registered = true;
            const auto state = registry.preparationState(QStringLiteral("desktop-share"));
            if (registered && (state == PreparationState::Ready || state == PreparationState::Failed))
                break;
            QTest::qWait(25);
        }
        bool registered = false;
        for (const auto &info : registry.plugins())
            if (info.id == QStringLiteral("desktop-share")) registered = true;
        if (!registered) {
            *error = QStringLiteral("desktop-share 未在超时前注册");
            return false;
        }
        if (registry.preparationState(QStringLiteral("desktop-share")) == PreparationState::Failed) {
            *error = QStringLiteral("desktop-share 准备失败：%1")
                         .arg(registry.preparationMessage(QStringLiteral("desktop-share")));
            return false;
        }
        if (registry.preparationState(QStringLiteral("desktop-share")) != PreparationState::Ready) {
            *error = QStringLiteral("desktop-share 未就绪：%1")
                         .arg(registry.preparationMessage(QStringLiteral("desktop-share")));
            return false;
        }
        auto created = registry.page(QStringLiteral("desktop-share/share"), &engine, &parent);
        if (!created.result.ok()) {
            *error = created.result.message;
            return false;
        }
        page = created.page;
        controller = page ? page->property("controller").value<QObject *>() : nullptr;
        if (!page || !controller) {
            *error = QStringLiteral("页面或控制器缺失");
            return false;
        }
        // The host sizes the page to the active view container and keeps it in
        // sync (AppController::navigate); the fixture does the same.
        page->setWidth(parent.width());
        page->setHeight(parent.height());
        QObject::connect(&parent, &QQuickItem::widthChanged, page,
                         [this] { page->setWidth(parent.width()); });
        QObject::connect(&parent, &QQuickItem::heightChanged, page,
                         [this] { page->setHeight(parent.height()); });
        loaded = true;
        return true;
    }
    bool unload(QString *error)
    {
        if (!loaded) return true;
        QPointer<QQuickItem> guard(page);
        QSignalSpy finished(&loader, &PluginLoader::unloadFinished);
        const auto requested = loader.unload(QStringLiteral("desktop-share"), registry);
        if (!requested.ok()) {
            *error = requested.message;
            return false;
        }
        QElapsedTimer timer;
        timer.start();
        while (finished.isEmpty() && timer.elapsed() < 30000) QTest::qWait(25);
        if (finished.isEmpty()) {
            *error = QStringLiteral("卸载未在超时前完成");
            return false;
        }
        const bool ok = finished.first().at(1).toBool();
        if (!ok) {
            *error = finished.first().at(2).toString();
            return false;
        }
        QElapsedTimer pageTimer;
        pageTimer.start();
        while (!guard.isNull() && pageTimer.elapsed() < 10000) QTest::qWait(25);
        if (!guard.isNull()) {
            *error = QStringLiteral("页面未释放");
            return false;
        }
        for (const auto &info : registry.plugins())
            if (info.id == QStringLiteral("desktop-share")) {
                *error = QStringLiteral("插件仍在注册表中");
                return false;
            }
        loaded = false;
        page = nullptr;
        controller = nullptr;
        return true;
    }
    QTemporaryDir install, settings;
    QQmlEngine engine;
    QQuickWindow window;
    QQuickItem parent;
    PluginRegistry registry;
    PluginLoader loader;
    QQuickItem *page = nullptr;
    QObject *controller = nullptr;
    bool loaded = false;
};
QStringList sourceKinds(QObject *controller)
{
    QStringList kinds;
    for (const auto &entry : controller->property("sources").toList()) {
        const auto kind = entry.toMap().value(QStringLiteral("kind")).toString();
        if (!kinds.contains(kind)) kinds.append(kind);
    }
    return kinds;
}
QString firstSourceValue(QObject *controller, const QString &kind)
{
    for (const auto &entry : controller->property("sources").toList()) {
        const auto map = entry.toMap();
        if (map.value(QStringLiteral("kind")).toString() == kind)
            return map.value(QStringLiteral("value")).toString();
    }
    return {};
}
template <class Predicate> bool waitUntil(Predicate predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) QTest::qWait(25);
    return predicate();
}
// A visible animated window. Capturing it makes the physical loopback test
// independent of whatever else is on the desktop, including a static desktop.
class AnimatedWidget final : public QWidget {
public:
    AnimatedWidget()
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAttribute(Qt::WA_NativeWindow);
    }
    void paintEvent(QPaintEvent *) override
    {
        ++m_frame;
        QPainter painter(this);
        painter.fillRect(rect(), QColor::fromHsv((m_frame * 7) % 360, 200, 220));
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        const auto travel = 0.15 + 0.7 * ((m_frame % 90) / 90.0);
        painter.drawEllipse(QPointF(width() * travel, height() / 2.0), 24, 24);
    }
    int frames() const { return m_frame; }

private:
    int m_frame = 0;
};

#ifdef Q_OS_WIN
// Module paths the host extracted from the plugin package. Used to prove the
// business DLL is really unmapped after an unload, not just deregistered.
QStringList mappedPluginCacheModules()
{
    QStringList result;
    const HANDLE snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            const auto path = QString::fromWCharArray(entry.szExePath);
            if (path.contains(QStringLiteral("zto-plugin-cache"), Qt::CaseInsensitive))
                result.append(path);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

int mediaWorkerProcessCount()
{
    int count = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return count;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID == GetCurrentProcessId()
                && QString::fromWCharArray(entry.szExeFile)
                    .compare(QStringLiteral("z2_media_worker.exe"), Qt::CaseInsensitive)
                == 0)
                ++count;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}
#endif
} // namespace

class DesktopSharePluginTests : public QObject {
    Q_OBJECT

private slots:
    void failedNetworkStopRetainsPluginAndProviderUntilRetry()
    {
        Fixture fixture;
        QString error;
        QVERIFY2(fixture.load(&error), qPrintable(error));
        const auto threads = fixture.controller->findChildren<QThread*>();
        QCOMPARE(threads.size(), 1);
        auto *thread = threads.first();
        QVERIFY(waitUntil([&] { return QAbstractEventDispatcher::instance(thread); }, 3000));
        std::atomic<bool> busy = false;
        QMetaObject::invokeMethod(QAbstractEventDispatcher::instance(thread), [&] {
            busy = true;
            QThread::msleep(5500);
        }, Qt::QueuedConnection);
        QVERIFY(waitUntil([&] { return busy.load(); }, 3000));
        QPointer<QQuickItem> page(fixture.page);
        QSignalSpy finished(&fixture.loader, &PluginLoader::unloadFinished);
        QElapsedTimer elapsed; elapsed.start();
        QVERIFY(fixture.loader.unload(QStringLiteral("desktop-share"), fixture.registry).ok());
        QVERIFY(elapsed.elapsed() < 100);
        QVERIFY(waitUntil([&] { return !finished.isEmpty(); }, 5300));
        QVERIFY(!finished.first().at(1).toBool());
        QVERIFY(page);
        QCOMPARE(fixture.controller->property("state").toString(), QStringLiteral("stop-failed"));
        bool consumer = false, provider = false;
        for (const auto &info : fixture.registry.plugins()) {
            consumer |= info.id == QStringLiteral("desktop-share");
            provider |= info.id == QStringLiteral("media");
        }
        QVERIFY(consumer); QVERIFY(provider);
        QVERIFY2(fixture.unload(&error), qPrintable(error));
        QVERIFY(!page);
    }

    void pageEnumeratesSourcesWithoutStartingCapture()
    {
        Fixture fixture;
        QString error;
        QVERIFY2(fixture.load(&error), qPrintable(error));
        fixture.window.show();
        QTest::qWait(200);
        auto *controller = fixture.controller;
        QCOMPARE(controller->property("state").toString(), QStringLiteral("idle"));
        QCOMPARE(controller->property("decodedFrames").toInt(), 0);
        QCOMPARE(controller->property("capturing").toBool(), false);
        QVERIFY(waitUntil([&] { return controller->property("sources").toList().size() > 0; }, 30000));
        // Only screen and window sources are offered; the synthetic/camera
        // sources of the media worker stay internal to its own tests.
        for (const auto &kind : sourceKinds(controller))
            QVERIFY2(kind == QStringLiteral("screen") || kind == QStringLiteral("window"),
                     qPrintable(kind));
        QVERIFY(!controller->property("statusText").toString().isEmpty());
        QTest::qWait(1500); // Opening the page must not start capture.
        QCOMPARE(controller->property("decodedFrames").toInt(), 0);
        QCOMPARE(controller->property("state").toString(), QStringLiteral("idle"));
        // The preview component is a page-owned visual child.
        auto *host = fixture.page->findChild<QQuickItem *>(QStringLiteral("desktopSharePreview"));
        QVERIFY(host);
        // VideoPreview is a QQuickPaintedItem without Q_OBJECT (public API), so
        // the structural check is the painted-item child added by the factory.
        bool hasPreview = false;
        QStringList childNames;
        for (auto *child : host->childItems()) {
            childNames.append(QString::fromLatin1(child->metaObject()->className()));
            if (dynamic_cast<QQuickPaintedItem *>(child)) hasPreview = true;
        }
        QVERIFY2(hasPreview, qPrintable(childNames.join(QStringLiteral(", "))));
        QVERIFY2(fixture.unload(&error), qPrintable(error));
    }

    void missingSourceRequiresReselection()
    {
        Fixture fixture;
        QString error;
        QVERIFY2(fixture.load(&error), qPrintable(error));
        fixture.window.show();
        QTest::qWait(200);
        auto *controller = fixture.controller;
        QVERIFY(waitUntil([&] { return controller->property("sources").toList().size() > 0; }, 30000));
        controller->setProperty("sourceValue", QStringLiteral("window:0000000000000000"));
        QMetaObject::invokeMethod(controller, "startCapture");
        QTest::qWait(600);
        QCOMPARE(controller->property("decodedFrames").toInt(), 0);
        QCOMPARE(controller->property("capturing").toBool(), false);
        QVERIFY2(controller->property("statusText").toString().contains(QStringLiteral("不可用")),
                 qPrintable(controller->property("statusText").toString()));
        QVERIFY2(fixture.unload(&error), qPrintable(error));
    }

    void localLoopbackBothProfiles_data()
    {
        QTest::addColumn<QString>("profile");
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<QString>("codec");
        QTest::newRow("720p30") << QStringLiteral("720p30") << 1280 << 720 << QStringLiteral("h264");
        QTest::newRow("1080p30") << QStringLiteral("1080p30") << 1920 << 1080 << QStringLiteral("h264");
        QTest::newRow("tiles720p30") << QStringLiteral("720p30") << 1280 << 720 << QStringLiteral("bgra-tiles");
        QTest::newRow("tiles1080p30") << QStringLiteral("1080p30") << 1920 << 1080 << QStringLiteral("bgra-tiles");
    }

    void localLoopbackBothProfiles()
    {
        if (!qEnvironmentVariableIntValue("ZTO_TEST_PHYSICAL_VIDEO"))
            QSKIP("Set ZTO_TEST_PHYSICAL_VIDEO=1 for real screen capture");
        QFETCH(QString, profile);
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(QString, codec);
        AnimatedWidget window;
        window.setWindowTitle(QStringLiteral("ZTOrbit shared capture fixture ") + QUuid::createUuid().toString(QUuid::Id128));
        window.resize(360, 240);
        window.show();
        QTest::qWait(120);
        window.hide();
        window.show();
        window.raise();
        QTest::qWait(300);
        QTimer animation;
        QObject::connect(&animation, &QTimer::timeout, &window, [&window] { window.update(); });
        animation.start(33);
        Fixture fixture;
        QString error;
        QVERIFY2(fixture.load(&error), qPrintable(error));
        fixture.window.show();
        QTest::qWait(200);
        // Keep the captured fixture clear of the page window: Windows Graphics
        // Capture is reliable for partially covered windows but this test must
        // not depend on occlusion behaviour.
        window.move(1450, 850);
        window.raise();
        window.activateWindow();
        QTest::qWait(200);
        auto *controller = fixture.controller;
        controller->setProperty("codec",codec);
        QVERIFY(waitUntil([&] { return controller->property("sources").toList().size() > 0; }, 30000));
        // Prefer capturing the animated fixture window itself: it changes even
        // when the rest of the desktop is static and does not depend on which
        // window is currently on top.
        QString source;
        for (const auto &entry : controller->property("sources").toList()) {
            const auto map = entry.toMap();
            if (map.value(QStringLiteral("kind")).toString() != QStringLiteral("window")) continue;
            if (map.value(QStringLiteral("description")).toString().contains(window.windowTitle())) {
                source = map.value(QStringLiteral("value")).toString();
                break;
            }
        }
        if (source.isEmpty()) {
            // Fallback: capture the screen showing the fixture. The page window
            // is hidden so it cannot cover the animated fixture.
            fixture.window.hide();
            QTest::qWait(250);
            const auto screenName = window.screen() ? window.screen()->name() : QString{};
            for (const auto &entry : controller->property("sources").toList()) {
                const auto map = entry.toMap();
                if (map.value(QStringLiteral("kind")).toString() != QStringLiteral("screen"))
                    continue;
                const auto value = map.value(QStringLiteral("value")).toString();
                if (screenName.isEmpty() || value == QStringLiteral("screen:") + screenName) {
                    source = value;
                    break;
                }
            }
            if (source.isEmpty()) source = firstSourceValue(controller, QStringLiteral("screen"));
        }
        QVERIFY2(!source.isEmpty(), "no screen or window source available");
        controller->setProperty("profile", profile);
        QCOMPARE(controller->property("profile").toString(), profile);
        controller->setProperty("sourceValue", source);
        QCOMPARE(controller->property("sourceValue").toString(), source);
        // Exercise restart on the same page/session and require continued
        // progress, so an initial handful of frames cannot hide a stalled queue.
        for (int cycle = 0; cycle < 2; ++cycle) {
            window.showNormal();
            window.raise();
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(window.windowHandle()));
            QVERIFY(QTest::qWaitForWindowActive(window.windowHandle()));
            QTest::qWait(100);
            QMetaObject::invokeMethod(controller, "startCapture");
            QVERIFY2(waitUntil([&] { return controller->property("decodedFrames").toInt() >= 5; }, 25000),
                     qPrintable(controller->property("statusText").toString() + QStringLiteral(" | ")
                                + controller->property("details").toString()));
            QCOMPARE(controller->property("frameWidth").toInt(), width);
            QCOMPARE(controller->property("frameHeight").toInt(), height);
            QVERIFY(controller->property("capturedFrames").toInt() > 0);
            const auto states = QStringList{QStringLiteral("live"), QStringLiteral("waiting"),
                                            QStringLiteral("interrupted")};
            QVERIFY2(states.contains(controller->property("state").toString()),
                     qPrintable(controller->property("state").toString()));
            QVERIFY(controller->property("details").toString().contains(codec == QStringLiteral("bgra-tiles")
                                                                          ? QStringLiteral("预览") : QStringLiteral("解码")));
            const int beforeProgress = controller->property("decodedFrames").toInt();
            QTest::qWait(2000);
            QVERIFY2(controller->property("decodedFrames").toInt() > beforeProgress,
                     qPrintable(controller->property("details").toString()));
            qInfo() << "loopback progress" << profile << "cycle" << cycle
                    << controller->property("details").toString();
            if (cycle == 1 && source.startsWith(QStringLiteral("window:"))) {
                QVERIFY(QTest::qWaitForWindowExposed(&fixture.window));
                const auto snapshot = fixture.window.grabWindow();
                const auto path = QString::fromUtf8(ZDS_PREVIEW_SCREENSHOT).chopped(4)
                                  + QLatin1Char('-') + profile
                                  + (codec == QStringLiteral("bgra-tiles") ? QStringLiteral("-tiles") : QString{})
                                  + QStringLiteral(".png");
                QVERIFY(!snapshot.isNull());
                QVERIFY(snapshot.save(path));
            }
            QMetaObject::invokeMethod(controller, "stopCapture");
            QVERIFY2(waitUntil(
                         [&] { return controller->property("state").toString() == QStringLiteral("idle"); },
                         10000),
                     qPrintable(controller->property("statusText").toString()));
            const int stopped = controller->property("decodedFrames").toInt();
            QTest::qWait(800);
            QCOMPARE(controller->property("decodedFrames").toInt(), stopped);
            QCOMPARE(controller->property("capturing").toBool(), false);
        }
        animation.stop();
        QVERIFY2(fixture.unload(&error), qPrintable(error));
    }

private:
    void exerciseStaticWindow(bool requireAutomaticColdFrame,
                              QString profile = QStringLiteral("1080p30"),
                              QString codec = QStringLiteral("bgra-tiles"), bool unloadActive = false)
    {
        if (!qEnvironmentVariableIntValue("ZTO_TEST_PHYSICAL_VIDEO"))
            QSKIP("Set ZTO_TEST_PHYSICAL_VIDEO=1 for real window capture");
        AnimatedWidget window;
        window.setWindowTitle(QStringLiteral("ZTOrbit static startup fixture ") + QUuid::createUuid().toString(QUuid::Id128));
        window.resize(320, 240);
        const auto available = window.screen()->availableGeometry();
        window.move(available.right() - window.width() - 40, available.bottom() - window.height() - 40);
        window.show();
        QTest::qWait(120);
        window.hide();
        window.show();
        window.raise();
        QTest::qWait(300);
        Fixture fixture;
        QString error;
        QVERIFY2(fixture.load(&error), qPrintable(error));
        fixture.window.resize(760, 480);
        fixture.window.setPosition(40,40);
        fixture.window.show();
        window.raise();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(window.windowHandle()));
        QVERIFY(QTest::qWaitForWindowActive(window.windowHandle()));
        QTest::qWait(500);
        auto *controller = fixture.controller;
        QVERIFY(waitUntil([&] { return !controller->property("sources").toList().isEmpty(); }, 30000));
        QString source;
        for (const auto &entry : controller->property("sources").toList()) {
            const auto map = entry.toMap();
            if (map.value(QStringLiteral("kind")).toString() == QStringLiteral("window") &&
                map.value(QStringLiteral("description")).toString().contains(window.windowTitle()))
                source = map.value(QStringLiteral("value")).toString();
        }
        QVERIFY2(!source.isEmpty(), "Static fixture window is not capturable");
        controller->setProperty("profile", profile);
        controller->setProperty("codec", codec);
        controller->setProperty("sourceValue", source);
        const int paintedBeforeStart = window.frames();
        for (int cycle = 0; cycle < (requireAutomaticColdFrame ? 2 : 1); ++cycle) {
        QMetaObject::invokeMethod(controller, "startCapture");
        if (!requireAutomaticColdFrame) {
            QTest::qWait(3500);
            QVERIFY(controller->property("capturing").toBool());
            if (!controller->property("decodedFrames").toInt()) {
                QCOMPARE(controller->property("state").toString(),QStringLiteral("waiting"));
                QVERIFY(controller->property("statusText").toString().contains(QStringLiteral("首帧")));
                // Provide exactly one initial source paint, then leave it
                // static. The media worker must supply the fresh baseline
                // from its cache without depending on another source frame.
                window.update();
            }
        }
        // The cold diagnostic supplies no paint; the regular case supplies
        // one initial paint and then verifies refresh from the cached canvas.
        QVERIFY2(waitUntil([&] { return controller->property("decodedFrames").toInt() >= 1; }, 5000),
                 qPrintable(controller->property("details").toString()));
        QCOMPARE(controller->property("frameWidth").toInt(), profile == QStringLiteral("720p30") ? 1280 : 1920);
        QCOMPARE(controller->property("frameHeight").toInt(), profile == QStringLiteral("720p30") ? 720 : 1080);
        if (requireAutomaticColdFrame) {
            QCOMPARE(window.frames(), paintedBeforeStart);
            auto *host = fixture.page->findChild<QQuickItem *>(QStringLiteral("desktopSharePreview"));
            QVERIFY(host);
            QQuickPaintedItem *preview = nullptr;
            for (auto *child : host->childItems())
                if (auto *painted = dynamic_cast<QQuickPaintedItem *>(child)) preview = painted;
            QVERIFY(preview);
            QImage picture(preview->size().toSize(), QImage::Format_RGB32);
            QVERIFY(!picture.isNull());
            QPainter painter(&picture);
            preview->paint(&painter);
            painter.end();
            const auto expected = QColor::fromHsv((paintedBeforeStart * 7) % 360, 200, 220);
            auto matches = [&](QColor pixel) {
                return std::abs(pixel.red() - expected.red()) < 30 &&
                    std::abs(pixel.green() - expected.green()) < 30 &&
                    std::abs(pixel.blue() - expected.blue()) < 30;
            };
            QVERIFY(matches(picture.pixelColor(picture.width()/2, picture.height()*2/3)));
            const auto bar = picture.pixelColor(picture.width()/20, picture.height()/2);
            QVERIFY(bar.red() < 12 && bar.green() < 12 && bar.blue() < 12);
            QRect content;
            for (int y = 0; y < picture.height(); y += 2)
                for (int x = 0; x < picture.width(); x += 2)
                    if (matches(picture.pixelColor(x,y))) content |= QRect(x,y,1,1);
            QVERIFY(content.height() > 60);
            QVERIFY2(std::abs(double(content.width())/content.height() - 4.0/3) < 0.08,
                     qPrintable(QStringLiteral("source colour extent %1x%2").arg(content.width()).arg(content.height())));
            if (cycle == 1) QVERIFY(picture.save(QString::fromUtf8(ZDS_PREVIEW_SCREENSHOT).chopped(4)
                + QStringLiteral("-static-") + profile + QLatin1Char('-') + codec + QStringLiteral(".png")));
        }
        // Exceed the ten-second source-unavailable threshold with a truly
        // static source; this also observes static waiting, without a repaint.
        const int quietMs = requireAutomaticColdFrame && cycle == 0 &&
            codec == QStringLiteral("bgra-tiles") && profile == QStringLiteral("1080p30") ? 12000 : 2000;
        QTest::qWait(quietMs);
        if (requireAutomaticColdFrame) QCOMPARE(window.frames(), paintedBeforeStart);
        QVERIFY(controller->property("capturing").toBool());
        QVERIFY(controller->property("state").toString() != QStringLiteral("failed"));
        qInfo() << "static startup" << "decodedFrames" << controller->property("decodedFrames").toInt()
                << "sourcePaints" << paintedBeforeStart << window.frames() << "cycle" << cycle
                << controller->property("details").toString();
        if (unloadActive && cycle == 1) {
            QVERIFY(controller->property("capturing").toBool());
            QVERIFY2(fixture.unload(&error), qPrintable(error));
            fixture.loader.requestShutdown();
            QVERIFY(waitUntil([&] { return fixture.loader.shutdownComplete(); }, 20000));
            QCOMPARE(window.frames(), paintedBeforeStart);
            return;
        }
        QMetaObject::invokeMethod(controller, "stopCapture");
        QVERIFY(waitUntil([&] { return controller->property("state").toString() == QStringLiteral("idle"); }, 10000));
        const int stopped = controller->property("decodedFrames").toInt();
        QTest::qWait(800);
        QCOMPARE(controller->property("decodedFrames").toInt(), stopped);
        }
        QVERIFY2(fixture.unload(&error), qPrintable(error));
    }

private slots:
    // Stage-6 remediation boundary: the complete requestStop -> media lease
    // close -> host unload chain with the real worker and the real DLL. A
    // fake-provider controller test can only prove the controller's bookkeeping;
    // this test proves the dynamic library is actually unmapped and the worker
    // is reclaimed.
    void unloadDuringRealCaptureClosesLeaseThenReleasesDllAndWorker()
    {
#ifdef Q_OS_WIN
        if (!qEnvironmentVariableIntValue("ZTO_TEST_PHYSICAL_VIDEO"))
            QSKIP("Set ZTO_TEST_PHYSICAL_VIDEO=1 for real window capture");
        AnimatedWidget window;
        window.setWindowTitle(QStringLiteral("ZTOrbit unload chain fixture ")
                              + QUuid::createUuid().toString(QUuid::Id128));
        window.resize(320, 240);
        const auto available = window.screen()->availableGeometry();
        window.move(available.right() - window.width() - 40,
                    available.bottom() - window.height() - 40);
        window.show();
        // The unattended launcher starts hidden. Windows consumes that flag on
        // the first ShowWindow; explicitly expose this real capture fixture,
        // as the static-window fixtures already do.
        window.hide();
        window.show();
        window.raise();
        QTest::qWait(300);
        Fixture fixture;
        QString error;
        QVERIFY2(fixture.load(&error), qPrintable(error));
        fixture.window.resize(760, 480);
        fixture.window.setPosition(40, 40);
        fixture.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(window.windowHandle()));
        QTest::qWait(400);
        auto *controller = fixture.controller;
        QVERIFY(waitUntil([&] { return !controller->property("sources").toList().isEmpty(); },
                          30000));
        QString source;
        for (const auto &entry : controller->property("sources").toList()) {
            const auto map = entry.toMap();
            if (map.value(QStringLiteral("kind")).toString() == QStringLiteral("window") &&
                map.value(QStringLiteral("description")).toString().contains(window.windowTitle()))
                source = map.value(QStringLiteral("value")).toString();
        }
        QVERIFY2(!source.isEmpty(), "synthetic window is not capturable");
        controller->setProperty("codec", QStringLiteral("bgra-tiles"));
        controller->setProperty("profile", QStringLiteral("720p30"));
        controller->setProperty("sourceValue", source);
        QMetaObject::invokeMethod(controller, "startCapture");
        QVERIFY2(waitUntil([&] { return controller->property("decodedFrames").toInt() >= 1; }, 20000),
                 qPrintable(controller->property("statusText").toString()));
        QVERIFY(controller->property("capturing").toBool());
        const auto modulesBefore = mappedPluginCacheModules();
        QVERIFY2(!modulesBefore.isEmpty(), "desktop-share DLL is not mapped from the host cache");
        // Record the actual loaded module binding: path and SHA-256 of the file
        // the host extracted from the authenticated package.
        for (const auto &modulePath : modulesBefore) {
            QFile moduleFile(modulePath);
            QVERIFY2(moduleFile.open(QIODevice::ReadOnly), qPrintable(modulePath));
            const auto digest =
                QCryptographicHash::hash(moduleFile.readAll(), QCryptographicHash::Sha256).toHex();
            qInfo().noquote() << "loaded module" << modulePath << "sha256"
                              << QString::fromLatin1(digest);
        }
        QVERIFY(mediaWorkerProcessCount() >= 1);

        // requestStop -> controller drain -> media lease close -> host unload.
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY2(fixture.unload(&error), qPrintable(error));
        const auto stopAndUnloadMs = elapsed.elapsed();
        // The library must be gone, not merely marked unloaded.
        const auto modulesAfter = mappedPluginCacheModules();
        QVERIFY2(modulesAfter.isEmpty(), qPrintable(modulesAfter.join(QLatin1Char(';'))));
        // The media provider keeps its worker alive until the host finishes the
        // shutdown and the lease owner is gone; only then is the worker reaped.
        QVERIFY(mediaWorkerProcessCount() >= 1);
        fixture.loader.requestShutdown();
        QVERIFY(waitUntil([&] { return fixture.loader.shutdownComplete(); }, 30000));
        QVERIFY2(waitUntil([] { return mediaWorkerProcessCount() == 0; }, 15000),
                 "media worker was not reaped after the provider was unloaded");
        QElapsedTimer quiet;
        quiet.start();
        while (quiet.elapsed() < 5000) {
            QTest::qWait(250);
            QCOMPARE(mediaWorkerProcessCount(), 0);
        }
        qInfo() << "unload chain stopAndUnloadMs" << stopAndUnloadMs
                << "mappedModulesBefore" << modulesBefore.size() << "after" << modulesAfter.size()
                << "workerReaped true quietMs" << quiet.elapsed();
#else
        QSKIP("Windows-only evidence");
#endif
    }

    void staticWindowStartsWithoutRepaint_data()
    {
        QTest::addColumn<QString>("profile");
        QTest::addColumn<QString>("codec");
        QTest::newRow("tiles1080p30") << QStringLiteral("1080p30") << QStringLiteral("bgra-tiles");
        QTest::newRow("tiles720p30") << QStringLiteral("720p30") << QStringLiteral("bgra-tiles");
        QTest::newRow("h2641080p30") << QStringLiteral("1080p30") << QStringLiteral("h264");
        QTest::newRow("h264720p30") << QStringLiteral("720p30") << QStringLiteral("h264");
    }
    void staticWindowStartsWithoutRepaint()
    {
        QFETCH(QString, profile);
        QFETCH(QString, codec);
        exerciseStaticWindow(true, profile, codec);
    }
    void staticWindowWaitsForInitialSourceFrame()
    {
        exerciseStaticWindow(false);
    }
    void unloadWhileStaticCaptureIsActive_data()
    {
        QTest::addColumn<QString>("codec");
        QTest::newRow("tiles") << QStringLiteral("bgra-tiles");
        QTest::newRow("h264") << QStringLiteral("h264");
    }
    void unloadWhileStaticCaptureIsActive()
    {
        QFETCH(QString, codec);
        exerciseStaticWindow(true, QStringLiteral("1080p30"), codec, true);
    }

    // ---- Stage 6: page lifecycle -------------------------------------------
    QVariantMap controllerStatistics(QObject *controller)
    {
        QVariantMap result;
        if (!QMetaObject::invokeMethod(controller, "statistics", Q_RETURN_ARG(QVariantMap, result)))
            return {};
        return result;
    }

    QString screenshotPath(const QString &suffix)
    {
        const auto directory = qEnvironmentVariable("ZDS_STAGE6_SCREENSHOTS");
        if (directory.isEmpty()) return {};
        QDir().mkpath(directory);
        return directory + QLatin1Char('/') + suffix;
    }

    void pageStartsDisconnectedWithoutNetworkOrCapture()
    {
        // First-run semantics: remove a preference file left by an earlier test
        // run so the page really starts without a configured address.
        QFile::remove(QCoreApplication::applicationDirPath()
                      + QStringLiteral("/plugins-data/id-desktop-share/preferences.json"));
        Fixture fixture;
        QString error;
        QVERIFY2(fixture.load(&error), qPrintable(error));
        fixture.window.show();
        QTest::qWait(200);
        auto *controller = fixture.controller;
        QCOMPARE(controller->property("phase").toString(), QStringLiteral("disconnected"));
        QCOMPARE(controller->property("phaseLabel").toString(), QStringLiteral("未连接"));
        QCOMPARE(controller->property("serverAddress").toString(), QString());
        QCOMPARE(controller->property("connected").toBool(), false);
        QCOMPARE(controller->property("capturing").toBool(), false);
        QCOMPARE(controller->property("canConnect").toBool(), true);
        QCOMPARE(controller->property("canStartShare").toBool(), false);
        QCOMPARE(controller->property("canJoin").toBool(), false);
        for (const auto *name : {"desktopShareServer", "desktopShareConnect", "desktopShareSource",
                                 "desktopShareRefresh", "desktopShareCodec", "desktopShareProfile",
                                 "desktopShareStart", "desktopShareStop", "desktopShareRoom",
                                 "desktopShareCopy", "desktopShareJoinCode", "desktopShareJoin",
                                 "desktopShareLeave", "desktopShareStatus", "desktopShareError",
                                 "desktopShareNextAction", "desktopSharePhaseLabel",
                                 "desktopShareViewers", "desktopSharePreview"}) {
            QVERIFY2(fixture.page->findChild<QObject *>(QString::fromLatin1(name)) != nullptr, name);
        }
        QCOMPARE(fixture.page->parent(), &fixture.parent);
        QCOMPARE(fixture.page->parentItem(), &fixture.parent);
        QCOMPARE(controller->parent(), fixture.page);
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        controller->setProperty("serverAddress",
                                QStringLiteral("127.0.0.1:%1").arg(listener.serverPort()));
        QVERIFY(waitUntil([&] { return controller->property("sources").toList().size() > 0; }, 30000));
        controller->setProperty("sourceValue", firstSourceValue(controller, QStringLiteral("screen")));
        QTest::qWait(1500);
        QCOMPARE(listener.hasPendingConnections(), false);
        QCOMPARE(controllerStatistics(controller).value(QStringLiteral("tcpRequestsSent")).toString(),
                 QStringLiteral("0"));
        QCOMPARE(controller->property("phase").toString(), QStringLiteral("disconnected"));
        QCOMPARE(controller->property("capturedFrames").toInt(), 0);
        QCOMPARE(controller->property("decodedFrames").toInt(), 0);
        QCOMPARE(controller->property("canStartShare").toBool(), false);
        const auto again = fixture.registry.page(QStringLiteral("desktop-share/share"),
                                                 &fixture.engine, &fixture.parent);
        QVERIFY2(again.result.ok(), qPrintable(again.result.message));
        QCOMPARE(again.page, fixture.page);
        QCOMPARE(again.page->property("controller").value<QObject *>(), controller);
        controller->setProperty("joinCode", QStringLiteral("00123456"));
        QVERIFY(fixture.registry.activateMenu(QStringLiteral("builtin/zerotier")).ok());
        QCOMPARE(controller->property("phase").toString(), QStringLiteral("disconnected"));
        QVERIFY(fixture.registry.activateMenu(QStringLiteral("desktop-share/share")).ok());
        QCOMPARE(fixture.registry.page(QStringLiteral("desktop-share/share"), &fixture.engine,
                                       &fixture.parent).page, fixture.page);
        QCOMPARE(controller->property("joinCode").toString(), QStringLiteral("00123456"));
        QCOMPARE(controller->property("sourceValue").toString(),
                 firstSourceValue(controller, QStringLiteral("screen")));
        QCOMPARE(controllerStatistics(controller).value(QStringLiteral("tcpRequestsSent")).toString(),
                 QStringLiteral("0"));
        QVERIFY2(fixture.unload(&error), qPrintable(error));
    }

    void menuSwitchRetainsLiveConnectionAndCancelReturnsToRealState()
    {
        Fixture fixture;
        QString error;
        QVERIFY2(fixture.load(&error), qPrintable(error));
        fixture.window.show();
        QTest::qWait(200);
        auto *controller = fixture.controller;
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost, 0));
        controller->setProperty("serverAddress",
                                QStringLiteral("127.0.0.1:%1").arg(silent.serverPort()));
        QVERIFY(QMetaObject::invokeMethod(controller, "connectServer"));
        QVERIFY(waitUntil([&] {
            return controller->property("phase").toString() == QStringLiteral("connecting");
        }, 2000));
        QCOMPARE(controller->property("canCancel").toBool(), true);
        QCOMPARE(controller->property("canConnect").toBool(), false);
        QCOMPARE(controller->property("canDisconnect").toBool(), false);
        QVERIFY(fixture.registry.activateMenu(QStringLiteral("builtin/zerotier")).ok());
        QCOMPARE(controller->property("phase").toString(), QStringLiteral("connecting"));
        QVERIFY(fixture.registry.activateMenu(QStringLiteral("desktop-share/share")).ok());
        QCOMPARE(controller->property("phase").toString(), QStringLiteral("connecting"));
        QCOMPARE(controller->property("canCancel").toBool(), true);
        QCOMPARE(fixture.registry.page(QStringLiteral("desktop-share/share"), &fixture.engine,
                                       &fixture.parent).page, fixture.page);
        QVERIFY(QMetaObject::invokeMethod(controller, "cancelOperation"));
        QVERIFY(waitUntil([&] {
            return controller->property("phase").toString() == QStringLiteral("disconnected");
        }, 3000));
        QCOMPARE(controller->property("canConnect").toBool(), true);
        QCOMPARE(controller->property("canCancel").toBool(), false);
        QCOMPARE(controller->property("capturing").toBool(), false);
        QCOMPARE(controller->property("publishing").toBool(), false);
        QVERIFY2(fixture.unload(&error), qPrintable(error));
    }

    void unloadWhileConnectingKeepsGuiHeartbeatAndDestroysPage()
    {
        Fixture fixture;
        QString error;
        QVERIFY2(fixture.load(&error), qPrintable(error));
        fixture.window.show();
        QTest::qWait(200);
        auto *controller = fixture.controller;
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost, 0));
        controller->setProperty("serverAddress",
                                QStringLiteral("127.0.0.1:%1").arg(silent.serverPort()));
        QVERIFY(QMetaObject::invokeMethod(controller, "connectServer"));
        QVERIFY(waitUntil([&] {
            return controller->property("phase").toString() == QStringLiteral("connecting");
        }, 2000));
        int heartbeat = 0;
        QTimer ticker;
        QObject::connect(&ticker, &QTimer::timeout, &fixture.window, [&] { ++heartbeat; });
        ticker.start(10);
        QPointer<QQuickItem> page(fixture.page);
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY2(fixture.unload(&error), qPrintable(error));
        ticker.stop();
        QVERIFY2(elapsed.elapsed() < 5000, qPrintable(QString::number(elapsed.elapsed())));
        QVERIFY2(heartbeat >= 5, qPrintable(QStringLiteral("GUI heartbeat stalled: %1").arg(heartbeat)));
        QVERIFY(page.isNull());
        QVERIFY(fixture.controller == nullptr);
        QVERIFY(fixture.page == nullptr);
    }

    void pageRendersInLightAndDarkAtDifferentSizes()
    {
        if (qEnvironmentVariableIsEmpty("ZDS_STAGE6_SCREENSHOTS"))
            QSKIP("Set ZDS_STAGE6_SCREENSHOTS to record theme/window-size evidence");
        for (const bool dark : {false, true}) {
            if (dark) {
                QPalette palette = QGuiApplication::palette();
                palette.setColor(QPalette::Window, QColor(32, 33, 36));
                palette.setColor(QPalette::WindowText, QColor(232, 234, 237));
                palette.setColor(QPalette::Base, QColor(24, 25, 28));
                palette.setColor(QPalette::Text, QColor(232, 234, 237));
                palette.setColor(QPalette::Button, QColor(48, 50, 54));
                palette.setColor(QPalette::ButtonText, QColor(232, 234, 237));
                palette.setColor(QPalette::Mid, QColor(90, 92, 96));
                palette.setColor(QPalette::Highlight, QColor(76, 140, 240));
                palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
                QGuiApplication::setPalette(palette);
            } else {
                QGuiApplication::setPalette(QApplication::style()->standardPalette());
            }
            Fixture fixture;
            QString error;
            QVERIFY2(fixture.load(&error), qPrintable(error));
            fixture.window.resize(dark ? 1280 : 900, dark ? 820 : 560);
            fixture.window.show();
            QTest::qWait(400);
            QVERIFY(QTest::qWaitForWindowExposed(&fixture.window));
            const auto shot = fixture.window.grabWindow();
            QVERIFY(!shot.isNull());
            const auto path = screenshotPath(QStringLiteral("page-%1-%2x%3.png")
                                                 .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
                                                 .arg(fixture.window.width())
                                                 .arg(fixture.window.height()));
            QVERIFY(shot.save(path));
            QCOMPARE(int(fixture.page->width()), fixture.window.width());
            QCOMPARE(int(fixture.page->height()), fixture.window.height());
            auto *preview = fixture.page->findChild<QQuickItem *>(QStringLiteral("desktopSharePreview"));
            QVERIFY(preview);
            QVERIFY(preview->width() > 0 && preview->height() > 0);
            QVERIFY2(fixture.unload(&error), qPrintable(error));
        }
    }

    // ---- Stage 6: process/thread lifecycle --------------------------------
    int currentThreadCount()
    {
#ifdef Q_OS_WIN
        int count = 0;
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return -1;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++count;
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return count;
#else
        return -1;
#endif
    }

    QSet<quint32> mediaWorkerPids()
    {
        QSet<quint32> result;
#ifdef Q_OS_WIN
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return result;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                const QString name = QString::fromWCharArray(entry.szExeFile);
                if (name.contains(QStringLiteral("zto_media_worker"), Qt::CaseInsensitive)
                    && entry.th32ParentProcessID == GetCurrentProcessId())
                    result.insert(entry.th32ProcessID);
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
#endif
        return result;
    }

    QStringList mediaWorkerProcesses()
    {
        QStringList result;
#ifdef Q_OS_WIN
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return result;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                const QString name = QString::fromWCharArray(entry.szExeFile);
                if (name.contains(QStringLiteral("zto_media_worker"), Qt::CaseInsensitive)
                    && entry.th32ParentProcessID == GetCurrentProcessId())
                    result.append(name + QLatin1Char(':') + QString::number(entry.th32ProcessID));
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
#endif
        return result;
    }

    void twentyLoadUnloadRounds()
    {
        const int rounds = qEnvironmentVariableIntValue("ZDS_STAGE6_LIFECYCLE_ROUNDS");
        if (rounds < 1) QSKIP("Set ZDS_STAGE6_LIFECYCLE_ROUNDS=20 to run the unload-round evidence");
        const QSet<quint32> baselineWorkerPids = mediaWorkerPids();
        QJsonArray samples;
        // The first round warms the QML engine, the loader thread pool and the
        // plugin tool runtime; those are process-wide and are not per-session
        // leaks. The stable baseline is taken after that warm-up, and every
        // following round must return to it.
        int warmBaseline = -1;
        int minThreads = 1 << 30;
        int maxThreads = 0;
        QList<int> threadSeries;
        for (int round = 1; round <= rounds; ++round) {
            int threadsAfterUnload = -1;
            int workersLoaded = -1;
            {
                Fixture fixture;
                QString error;
                QVERIFY2(fixture.load(&error),
                         qPrintable(QStringLiteral("round %1: %2").arg(round).arg(error)));
                QVERIFY(waitUntil([&] { return fixture.controller->property("sources").toList().size() > 0; }, 30000));
                QCOMPARE(fixture.controller->property("phase").toString(),
                         QStringLiteral("disconnected"));
                QCOMPARE(fixture.controller->property("capturedFrames").toInt(), 0);
                QCOMPARE(fixture.controller->property("decodedFrames").toInt(), 0);
                workersLoaded = mediaWorkerPids().size();
                QVERIFY2(fixture.unload(&error),
                         qPrintable(QStringLiteral("round %1: %2").arg(round).arg(error)));
                QVERIFY(fixture.page == nullptr);
                QVERIFY(fixture.controller == nullptr);
                threadsAfterUnload = currentThreadCount();
            }
            // The fixture destructor unloaded the media provider (no other
            // consumer in this process) and waited for its worker to exit. A
            // worker spawned by this round must not survive it; workers that
            // already existed before this test are ignored.
            QVERIFY2(waitUntil([&] { return (mediaWorkerPids() - baselineWorkerPids).isEmpty(); }, 15000),
                     qPrintable(QStringLiteral("round %1 left new media workers: %2")
                                    .arg(round)
                                    .arg(mediaWorkerProcesses().join(QLatin1Char(',')))));
            QSet<quint32> leaked = mediaWorkerPids() - baselineWorkerPids;
            QTest::qWait(100);
            const int threadsAfterTeardown = currentThreadCount();
            if (warmBaseline < 0) {
                warmBaseline = threadsAfterTeardown;
            } else {
                threadSeries.append(threadsAfterTeardown);
                minThreads = qMin(minThreads, threadsAfterTeardown);
                maxThreads = qMax(maxThreads, threadsAfterTeardown);
                // Qt's global thread pool lazily retires and recreates idle
                // threads, so the plateau may move inside a fixed allowance; it
                // must never exceed that allowance.
                QVERIFY2(threadsAfterTeardown <= warmBaseline + 6,
                         qPrintable(QStringLiteral("round %1 threads %2 vs warm baseline %3")
                                        .arg(round).arg(threadsAfterTeardown).arg(warmBaseline)));
            }
            samples.append(QJsonObject{{QStringLiteral("round"), round},
                                       {QStringLiteral("workersLoaded"), workersLoaded},
                                       {QStringLiteral("threadsAfterUnload"), threadsAfterUnload},
                                       {QStringLiteral("threadsAfterTeardown"), threadsAfterTeardown},
                                       {QStringLiteral("mediaWorkersAfterTeardown"),
                                        mediaWorkerPids().size()},
                                       {QStringLiteral("leakedWorkers"), leaked.size()}});
        }
        if (threadSeries.size() >= 6) {
            // Early vs late plateau: idle pool threads fluctuate, so the check is
            // that the late plateau is not larger than the early one.
            int earlyMax = 0;
            for (int index = 0; index < 3; ++index) earlyMax = qMax(earlyMax, threadSeries.at(index));
            int lateMax = 0;
            for (int index = threadSeries.size() - 3; index < threadSeries.size(); ++index)
                lateMax = qMax(lateMax, threadSeries.at(index));
            QVERIFY2(lateMax <= earlyMax + 2,
                     qPrintable(QStringLiteral("late thread plateau %1 exceeds early %2")
                                    .arg(lateMax).arg(earlyMax)));
        }
        const auto path = qEnvironmentVariable("ZDS_STAGE6_LIFECYCLE_JSON");
        if (!path.isEmpty()) {
            QSaveFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QJsonArray series;
            for (const int value : threadSeries) series.append(value);
            QJsonObject document{{QStringLiteral("rounds"), rounds},
                                 {QStringLiteral("warmBaselineThreads"), warmBaseline},
                                 {QStringLiteral("minThreads"), minThreads},
                                 {QStringLiteral("maxThreads"), maxThreads},
                                 {QStringLiteral("threadSeriesAfterWarmup"), series},
                                 {QStringLiteral("baselineMediaWorkers"), baselineWorkerPids.size()},
                                 {QStringLiteral("samples"), samples}};
            file.write(QJsonDocument(document).toJson(QJsonDocument::Indented));
            QVERIFY(file.commit());
        }
    }

    // The provider exits when its last consumer is unloaded and stays alive
    // while another consumer still depends on it.
    void mediaProviderFollowsConsumerLifetime()
    {
        const auto baselineWorkers = mediaWorkerProcesses().size();
        QTemporaryDir install;
        QVERIFY(install.isValid());
        QVERIFY(QFile::copy(DESKTOP_SHARE_PLUGIN,
                            install.filePath(QStringLiteral("desktop-share.z2plugin"))));
        QVERIFY(QFile::copy(MEDIA_PLUGIN, install.filePath(QStringLiteral("media.z2plugin"))));
        QFile::copy(VOICE_PLUGIN, install.filePath(QStringLiteral("voice-chat.z2plugin")));
        QQmlEngine engine;
        QQuickWindow window;
        QQuickItem parent;
        parent.setParentItem(window.contentItem());
        PluginRegistry registry;
        PluginLoader loader;
        loader.scan(install.path(), registry);
        auto waitReady = [&](const QString &id, int timeout) {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < timeout) {
                bool present = false;
                for (const auto &info : registry.plugins())
                    if (info.id == id) present = true;
                const auto state = registry.preparationState(id);
                if (present && (state == PreparationState::Ready
                                || state == PreparationState::Failed))
                    return state == PreparationState::Ready;
                QTest::qWait(25);
            }
            return false;
        };
        QVERIFY2(waitReady(QStringLiteral("desktop-share"), 30000), "desktop-share did not prepare");
        const bool voiceReady = waitReady(QStringLiteral("voice-chat"), 30000);
        if (!voiceReady) {
            qWarning() << "voice-chat is not available in this environment; provider retention is"
                          " covered by the media project's two-consumer integration test";
            loader.requestShutdown();
            QVERIFY(waitUntil([&] { return loader.shutdownComplete(); }, 30000));
            QSKIP("voice-chat did not prepare; other-consumer retention not executed here");
        }
        QVERIFY2(!mediaWorkerProcesses().isEmpty(), "media worker did not start");
        const auto created = registry.page(QStringLiteral("desktop-share/share"), &engine, &parent);
        QVERIFY2(created.result.ok(), qPrintable(created.result.message));
        QPointer<QQuickItem> page(created.page);
        QSignalSpy finished(&loader, &PluginLoader::unloadFinished);
        QVERIFY(loader.unload(QStringLiteral("desktop-share"), registry).ok());
        QVERIFY(waitUntil([&] {
            for (const auto &info : registry.plugins())
                if (info.id == QStringLiteral("desktop-share")) return false;
            return true;
        }, 30000));
        // The voice consumer still depends on media: the provider and its worker
        // must stay alive and media must not be unloadable yet.
        QVERIFY2(mediaWorkerProcesses().size() > baselineWorkers,
                 "media worker exited although another consumer still depends on it");
        const auto impact = loader.unloadImpact(QStringLiteral("media"));
        QVERIFY2(impact.contains(QStringLiteral("voice-chat")), qPrintable(impact.join(QLatin1Char(','))));
        bool mediaRegistered = false;
        for (const auto &info : registry.plugins())
            if (info.id == QStringLiteral("media")) mediaRegistered = true;
        QVERIFY(mediaRegistered);
        // Unloading the dependent first releases the provider; only then may the
        // media provider and its worker exit.
        QVERIFY(loader.unload(QStringLiteral("voice-chat"), registry).ok());
        QVERIFY(waitUntil([&] {
            for (const auto &info : registry.plugins())
                if (info.id == QStringLiteral("voice-chat")) return false;
            return true;
        }, 30000));
        loader.requestShutdown();
        QVERIFY(waitUntil([&] { return loader.shutdownComplete(); }, 60000));
        QVERIFY2(waitUntil([&] { return mediaWorkerProcesses().isEmpty(); }, 20000),
                 qPrintable(mediaWorkerProcesses().join(QLatin1Char(','))));
        Q_UNUSED(page);
        Q_UNUSED(finished);
    }

    void waitingStateForStaticWindow()
    {
        if (!qEnvironmentVariableIntValue("ZTO_TEST_PHYSICAL_VIDEO"))
            QSKIP("Set ZTO_TEST_PHYSICAL_VIDEO=1 for real window capture");
        AnimatedWidget window;
        window.setWindowTitle(QStringLiteral("ZTOrbit waiting fixture ") + QUuid::createUuid().toString(QUuid::Id128));
        window.resize(320, 240);
        window.show();
        QTest::qWait(120);
        window.hide();
        window.show();
        window.raise();
        QTest::qWait(300);
        QTimer animation;
        QObject::connect(&animation, &QTimer::timeout, &window, [&window] { window.update(); });
        animation.start(33);
        Fixture fixture;
        QString error;
        QVERIFY2(fixture.load(&error), qPrintable(error));
        fixture.window.show();
        QTest::qWait(200);
        window.move(1450, 850);
        window.raise();
        window.activateWindow();
        QTest::qWait(200);
        auto *controller = fixture.controller;
        QVERIFY(waitUntil([&] { return controller->property("sources").toList().size() > 0; }, 30000));
        QString source;
        for (const auto &entry : controller->property("sources").toList()) {
            const auto map = entry.toMap();
            if (map.value(QStringLiteral("kind")).toString() != QStringLiteral("window")) continue;
            if (map.value(QStringLiteral("description")).toString().contains(window.windowTitle())) {
                source = map.value(QStringLiteral("value")).toString();
                break;
            }
        }
        if (source.isEmpty()) {
            QString ignored;
            fixture.unload(&ignored);
            QSKIP("Fixture window is not capturable on this platform");
        }
        controller->setProperty("sourceValue", source);
        QMetaObject::invokeMethod(controller, "startCapture");
        QVERIFY2(waitUntil([&] { return controller->property("decodedFrames").toInt() >= 10; }, 25000),
                 qPrintable(controller->property("statusText").toString()));
        window.raise();
        QTest::qWait(400);
        // Evidence screenshot: the page renders the H.264 loopback of the
        // synthetic fixture window only, never the user's desktop content.
        QVERIFY(QTest::qWaitForWindowExposed(&fixture.window));
        const auto snapshot = fixture.window.grabWindow();
        const auto saved = !snapshot.isNull()
                           && snapshot.save(QString::fromUtf8(ZDS_PREVIEW_SCREENSHOT));
        qWarning() << "desktop-share preview screenshot" << snapshot.size()
                   << QString::fromUtf8(ZDS_PREVIEW_SCREENSHOT) << saved
                   << controller->property("details").toString();
        animation.stop();
        // Minimizing the captured window stops new frames without closing it:
        // the controller must show a waiting state, not a source failure.
        window.showMinimized();
        QVERIFY2(waitUntil(
                     [&] { return controller->property("state").toString() == QStringLiteral("waiting"); },
                     15000),
                 qPrintable(controller->property("state").toString()));
        QVERIFY2(controller->property("statusText").toString().contains(QStringLiteral("等待新画面")),
                 qPrintable(controller->property("statusText").toString()));
        QCOMPARE(controller->property("capturing").toBool(), true);
        QVERIFY2(controller->property("state").toString() != QStringLiteral("failed"),
                 qPrintable(controller->property("statusText").toString()));
        window.showNormal();
        window.raise();
        window.activateWindow();
        window.move(1450, 850);
        animation.start(30);
        QVERIFY2(waitUntil(
                     [&] { return controller->property("state").toString() == QStringLiteral("live"); },
                     10000),
                 qPrintable(controller->property("statusText").toString()));
        animation.stop();
        QMetaObject::invokeMethod(controller, "stopCapture");
        QVERIFY2(waitUntil(
                     [&] { return controller->property("state").toString() == QStringLiteral("idle"); },
                     10000),
                 qPrintable(controller->property("statusText").toString()));
        QVERIFY2(fixture.unload(&error), qPrintable(error));
    }
};

int main(int argc, char **argv)
{
    // Software rendering is the host default; ZTO_TEST_GPU=1 exercises the
    // same page and preview through the GPU Quick backend.
    if (qEnvironmentVariableIntValue("ZTO_TEST_GPU")) {
        qunsetenv("QT_QUICK_BACKEND");
        if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND")) qputenv("QSG_RHI_BACKEND", "d3d11");
    } else {
        qputenv("QT_QUICK_BACKEND", "software");
    }
#ifdef Q_OS_WIN
    qputenv("QT_QPA_FONTDIR", (qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts").toLocal8Bit());
#endif
    QApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
#ifdef Q_OS_WIN
    QFontDatabase::addApplicationFont(
        qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/msyh.ttc");
    app.setFont(QFont(QStringLiteral("Microsoft YaHei"), 10));
#endif
    DesktopSharePluginTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "DesktopSharePluginTests.moc"
