#include "../../client/main/plugins/PluginLoader.h"
#include "../../client/main/plugins/PluginRegistry.h"

#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <QtMath>

#include <cmath>
#include <cstdio>
#include <functional>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace vusiif::zto;
using namespace vusiif::zto::plugins;

namespace {

QString env(const char *name, const QString &fallback = {})
{
    return qEnvironmentVariable(name, fallback);
}

int envInt(const char *name, int fallback = 0)
{
    bool ok = false;
    const int value = env(name).toInt(&ok);
    return ok ? value : fallback;
}

QColor tagColor(const QString &tag)
{
    if (tag == QStringLiteral("A")) return QColor(230, 30, 200);   // magenta
    if (tag == QStringLiteral("B")) return QColor(20, 190, 60);    // green
    if (tag == QStringLiteral("C")) return QColor(240, 140, 10);   // orange
    if (tag == QStringLiteral("D")) return QColor(30, 90, 230);    // blue
    return QColor(180, 180, 180);
}

void pump(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
}

bool waitFor(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return predicate();
}

void writeJson(const QString &path, const QVariantMap &values)
{
    if (path.isEmpty()) return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return;
    file.write(QJsonDocument(QJsonObject::fromVariantMap(values)).toJson(QJsonDocument::Indented));
    file.commit();
}

QString readTrimmed(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll()).trimmed();
}

void writeTrimmed(const QString &path, const QString &text)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return;
    file.write(text.toUtf8());
    file.commit();
}

// Test-only resource observation: append one compact JSON line per sample so a
// long RSS run can record business-queue bounds without retaining the harness's
// own per-second QVariantMap history. The product pipeline is untouched.
void appendObservationLine(const QString &path, const QVariantMap &values)
{
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::WriteOnly)) return;
    file.write(QJsonDocument(QJsonObject::fromVariantMap(values)).toJson(QJsonDocument::Compact));
    file.write("\n");
    file.flush();
}

// Test-only full time code. A 32-cell barcode in the top strip carries 24 bits
// of the publisher's absolute Unix millisecond clock (mod 2^24, about 4.6 h,
// enough for one run and far wider than the 10 s first-frame limit). The viewer
// decodes the exact value instead of a one-second phase, so delays above one
// second are not aliased. Cell size is chosen so the code survives preview
// scaling down to roughly 300 px content width.
constexpr int kTimeCodeCells = 32;
constexpr int kTimeCodeDataBits = 24;
constexpr qint64 kTimeCodeMask = (qint64(1) << kTimeCodeDataBits) - 1;
constexpr int kTimeCodeStripDivisor = 6;

bool timeCodeCellWhite(int index, qint64 value)
{
    if (index == 0) return true;
    if (index <= kTimeCodeDataBits) {
        const int bit = kTimeCodeDataBits - index;
        return ((value >> bit) & 1) != 0;
    }
    return ((index - kTimeCodeDataBits - 1) % 2) == 0;
}

void drawTimeCodeStrip(QPainter &painter, const QRect &strip, qint64 value)
{
    value &= kTimeCodeMask;
    painter.fillRect(strip, Qt::black);
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < kTimeCodeCells; ++i) {
        const int x0 = strip.left() + (strip.width() * i) / kTimeCodeCells;
        const int x1 = strip.left() + (strip.width() * (i + 1)) / kTimeCodeCells;
        painter.fillRect(QRect(x0, strip.top(), qMax(1, x1 - x0), strip.height()),
                         timeCodeCellWhite(i, value) ? Qt::white : Qt::black);
    }
}

QImage renderTimeCodeImage(qint64 value, int width = 480, int height = 270)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(230, 30, 200));
    QPainter painter(&image);
    drawTimeCodeStrip(painter, QRect(0, 0, width, height / kTimeCodeStripDivisor), value);
    painter.end();
    return image;
}

// Locates the saturated source content in one row and returns its horizontal
// bounds. The black letterbox/window frame and the white grid/text are excluded.
bool timeCodeContentBounds(const QImage &image, int y, int *left, int *width)
{
    if (image.isNull() || y < 0 || y >= image.height()) return false;
    int first = -1;
    int last = -1;
    for (int x = 0; x < image.width(); ++x) {
        const auto colour = image.pixelColor(x, y);
        if (colour.saturation() > 40 && colour.lightness() > 24) {
            if (first < 0) first = x;
            last = x;
        }
    }
    if (first < 0 || last - first < 64) return false;
    *left = first;
    *width = last - first + 1;
    return true;
}

bool decodeTimeCodeRow(const QImage &image, int y, int left, int width, qint64 *value)
{
    bool cells[kTimeCodeCells];
    for (int i = 0; i < kTimeCodeCells; ++i) {
        const int x = left + (width * (2 * i + 1)) / (2 * kTimeCodeCells);
        cells[i] = image.pixelColor(qBound(0, x, image.width() - 1), y).lightness() > 150;
    }
    if (!cells[0]) return false;
    for (int i = kTimeCodeDataBits + 1; i < kTimeCodeCells; ++i) {
        const bool expected = ((i - kTimeCodeDataBits - 1) % 2) == 0;
        if (cells[i] != expected) return false;
    }
    qint64 decoded = 0;
    for (int i = 1; i <= kTimeCodeDataBits; ++i)
        decoded = (decoded << 1) | (cells[i] ? 1 : 0);
    *value = decoded;
    return true;
}

bool decodeTimeCode(const QImage &image, qint64 *value)
{
    if (image.isNull()) return false;
    // The barcode strip itself has no saturated pixels, so locate the source
    // content from a row below the strip and reuse those bounds for the code.
    int contentLeft = 0;
    int contentWidth = 0;
    bool haveBounds = false;
    for (double fraction : {0.60, 0.50, 0.70, 0.80}) {
        const int row = qBound(0, int(image.height() * fraction), image.height() - 1);
        if (timeCodeContentBounds(image, row, &contentLeft, &contentWidth)) {
            haveBounds = true;
            break;
        }
    }
    if (!haveBounds) return false;
    const int limit = qMin(image.height() - 1, int(image.height() * 0.5));
    for (int y = 1; y <= limit; ++y) {
        qint64 decoded = 0;
        if (!decodeTimeCodeRow(image, y, contentLeft, contentWidth, &decoded)) continue;
        // Require the same value on a neighbouring row so a single text edge
        // cannot be mistaken for the barcode.
        if (y + 1 <= limit) {
            qint64 decoded2 = 0;
            if (decodeTimeCodeRow(image, y + 1, contentLeft, contentWidth, &decoded2)
                && decoded2 == decoded) {
                *value = decoded;
                return true;
            }
        }
        *value = decoded;
        return true;
    }
    return false;
}

// Signed latency from a 24-bit publisher timestamp. Values beyond half the
// modulus are treated as clock skew, and latencies over one minute are rejected
// instead of being silently folded.
bool timeCodeLatencyMs(qint64 localMs, qint64 decodedMs, qint64 *latencyMs)
{
    qint64 diff = (localMs - decodedMs) & kTimeCodeMask;
    if (diff > (qint64(1) << (kTimeCodeDataBits - 1))) diff -= (qint64(1) << kTimeCodeDataBits);
    if (diff < 0 || diff > 60000) return false;
    *latencyMs = diff;
    return true;
}

// A synthetic capture source: a saturated tag colour with a moving white ball.
// Only this window is ever captured or shown in evidence screenshots.
class TagWidget final : public QWidget {
public:
    explicit TagWidget(const QColor &color)
    {
        m_color = color;
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAttribute(Qt::WA_NativeWindow);
        setWindowTitle(QStringLiteral("ZTOrbit stage4 fixture %1")
                           .arg(QString::number(m_frame) + QStringLiteral("-") + color.name()));
        m_timer.setInterval(envInt("ZDS_E2E_TICK_MS", 33));
        m_timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] {
            ++m_tick;
            update();
        });
    }
    void start() { m_timer.start(); }
    void stop() { m_timer.stop(); }
    void freeze() { m_timer.stop(); }
    // Test-only unique marker: one repaint of the same frozen source with a new
    // tag colour. Used to prove recovery delivered new content after a fault.
    void setTagColour(const QColor &colour)
    {
        m_color = colour;
        update();
    }
    // Test-only end-to-end latency indicator: a barcode carrying the complete
    // publisher Unix-millisecond timestamp, so delays above one second are
    // measurable and testable.
    void enableLatencyBar() { m_latencyBar = true; }
    // Test-only text/grid content for the visual-quality case.
    void enableGrid() { m_grid = true; }
    QString title() const { return windowTitle(); }
    int paints() const { return m_paints; }
    int ticks() const { return m_tick; }
    int closeEvents() const { return m_closeEvents; }
    quintptr nativeHandle() const { return winId(); }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        ++m_closeEvents;
        QWidget::closeEvent(event);
    }
    void paintEvent(QPaintEvent *) override
    {
        ++m_frame;
        ++m_paints;
        QPainter painter(this);
        painter.fillRect(rect(), m_color);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        const auto travel = 0.12 + 0.76 * ((m_tick % 80) / 80.0);
        painter.drawEllipse(QPointF(width() * travel, height() * 0.5), width() * 0.06,
                            width() * 0.06);
        painter.setBrush(Qt::black);
        painter.drawRect(0, height() - height() / 8, width(), height() / 8);
        if (m_latencyBar) {
            painter.setBrush(Qt::NoBrush);
            drawTimeCodeStrip(painter, QRect(0, 0, width(), height() / kTimeCodeStripDivisor),
                              QDateTime::currentMSecsSinceEpoch() & kTimeCodeMask);
        }
        if (m_grid) {
            painter.setBrush(Qt::NoBrush);
            QPen pen(QColor(255, 255, 255, 170));
            pen.setWidth(1);
            painter.setPen(pen);
            for (int x = 0; x <= width(); x += qMax(8, width() / 16))
                painter.drawLine(x, height() / 4, x, height() * 3 / 4);
            for (int y = height() / 4; y <= height() * 3 / 4; y += qMax(6, height() / 12))
                painter.drawLine(0, y, width(), y);
            QFont font = painter.font();
            font.setPixelSize(qMax(10, height() / 10));
            font.setBold(true);
            painter.setFont(font);
            painter.setPen(Qt::black);
            painter.drawText(QRect(0, height() / 3, width(), height() / 6),
                             Qt::AlignCenter,
                             QStringLiteral("ZTOrbit 文字 0123456789 frame %1").arg(m_frame));
            painter.setPen(Qt::white);
            painter.drawText(QRect(1, height() / 3 + 1, width(), height() / 6),
                             Qt::AlignCenter,
                             QStringLiteral("ZTOrbit 文字 0123456789 frame %1").arg(m_frame));
        }
    }

private:
    QColor m_color;
    QTimer m_timer;
    int m_tick = 0;
    int m_frame = 0;
    int m_paints = 0;
    int m_closeEvents = 0;
    bool m_latencyBar = false;
    bool m_grid = false;
};

class Fixture {
public:
    Fixture()
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        if (settings.isValid())
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        window.setColor(Qt::black);
        window.resize(900, 620);
        window.setPosition(20, 20);
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
        while (!loader.shutdownComplete() && timer.elapsed() < 30000) pump(20);
    }
    bool load(QString *error)
    {
        if (!install.isValid() || !settings.isValid()) {
            *error = QStringLiteral("临时目录不可用");
            return false;
        }
        if (!QFile::copy(env("ZDS_E2E_DESKTOP_PACKAGE", DESKTOP_SHARE_PLUGIN), install.filePath(QStringLiteral("desktop-share.z2plugin")))) {
            *error = QStringLiteral("复制 desktop-share 包失败");
            return false;
        }
        if (!QFile::copy(env("ZDS_E2E_MEDIA_PACKAGE", MEDIA_PLUGIN), install.filePath(QStringLiteral("media.z2plugin")))) {
            *error = QStringLiteral("复制 media 包失败");
            return false;
        }
        loader.scan(install.path(), registry);
        if (!waitFor(
                [this] {
                    bool registered = false;
                    for (const auto &info : registry.plugins())
                        if (info.id == QStringLiteral("desktop-share")) registered = true;
                    const auto state = registry.preparationState(QStringLiteral("desktop-share"));
                    return registered
                        && (state == PreparationState::Ready || state == PreparationState::Failed);
                },
                90000)) {
            *error = QStringLiteral("桌面共享插件准备超时");
            return false;
        }
        if (registry.preparationState(QStringLiteral("desktop-share")) != PreparationState::Ready) {
            *error = QStringLiteral("桌面共享插件未就绪：%1")
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
        page->setWidth(parent.width());
        page->setHeight(parent.height());
        QObject::connect(&parent, &QQuickItem::widthChanged, page,
                         [this] { page->setWidth(parent.width()); });
        QObject::connect(&parent, &QQuickItem::heightChanged, page,
                         [this] { page->setHeight(parent.height()); });
        loaded = true;
        const auto prepared = env("ZDS_E2E_PREPARED_FILE");
        const auto gate = env("ZDS_E2E_START_GATE");
        if (!prepared.isEmpty()) {
            const auto pid = QString::number(QCoreApplication::applicationPid());
            writeTrimmed(prepared, pid);
            if (readTrimmed(prepared) != pid) {
                *error = QStringLiteral("不能记录测试准备状态");
                return false;
            }
        }
        if (!gate.isEmpty() && !waitFor([&] { return QFile::exists(gate); }, 120000)) {
            *error = QStringLiteral("测试准备屏障超时（未发起网络／采集）");
            return false;
        }
        return true;
    }
    bool unload(QString *error)
    {
        if (!loaded) return true;
        std::fprintf(stderr, "[e2e] unload: request\n");
        std::fflush(stderr);
        if (!loader.unload(QStringLiteral("desktop-share"), registry).ok()) {
            *error = QStringLiteral("卸载请求失败");
            return false;
        }
        if (!waitFor(
                [this] {
                    for (const auto &info : registry.plugins())
                        if (info.id == QStringLiteral("desktop-share")) return false;
                    return true;
                },
                40000)) {
            *error = QStringLiteral("卸载未完成");
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

QImage previewImage(QQuickItem *page)
{
    if (!page) return {};
    auto *host = page->findChild<QQuickItem *>(QStringLiteral("desktopSharePreview"));
    if (!host) return {};
    QQuickPaintedItem *preview = nullptr;
    for (auto *child : host->childItems())
        if (auto *painted = dynamic_cast<QQuickPaintedItem *>(child)) preview = painted;
    if (!preview) return {};
    QImage picture(preview->size().toSize(), QImage::Format_RGB32);
    if (picture.isNull()) return {};
    QPainter painter(&picture);
    preview->paint(&painter);
    painter.end();
    return picture;
}

double colourFraction(const QImage &image, const QColor &expected)
{
    if (image.isNull()) return 0;
    int matches = 0;
    int samples = 0;
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4) {
            const auto pixel = image.pixelColor(x, y);
            ++samples;
            if (std::abs(pixel.red() - expected.red()) < 40
                && std::abs(pixel.green() - expected.green()) < 40
                && std::abs(pixel.blue() - expected.blue()) < 40)
                ++matches;
        }
    }
    return samples == 0 ? 0 : double(matches) / samples;
}

struct Result {
    bool ok = true;
    QString error;
    QVariantMap stats;
    void require(bool condition, const QString &message)
    {
        if (!condition && ok) {
            ok = false;
            error = message;
        }
    }
};

QVariantMap ownedObservation(const QVariantMap &value)
{
    // QVariant/QString copies can retain QStringLiteral storage in the plugin
    // DLL. Decode serialized bytes into Qt-owned storage before retaining any
    // snapshot across unload (including aliases in local sample lists).
    return QJsonDocument::fromJson(QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact))
        .toVariant().toMap();
}

QVariantMap controllerStats(QObject *controller)
{
    QVariantMap result;
    if (!controller) return result;
    QVariantMap returned;
    if (QMetaObject::invokeMethod(controller, "statistics", Q_RETURN_ARG(QVariantMap, returned)))
        return ownedObservation(returned);
    return result;
}

int finish(Result &result, const QString &outPath, Fixture &fixture)
{
    // Plugin-returned QString literals may borrow read-only DLL storage. Persist
    // the complete metric snapshot while that DLL is still mapped. Teardown's
    // outcome is an independent record containing only EXE-owned values.
    const auto unloadRecord = QStringLiteral("unload-") + QFileInfo(outPath).fileName();
    result.stats.insert(QStringLiteral("unloadPending"), true);
    result.stats.insert(QStringLiteral("unloadRecord"), unloadRecord);
    // Test-only DPI evidence: logical window size and the Qt device pixel ratio
    // show whether a QT_SCALE_FACTOR substitute actually changed the UI scale.
    result.stats.insert(QStringLiteral("devicePixelRatio"),
                        fixture.window.devicePixelRatio());
    result.stats.insert(QStringLiteral("windowLogicalWidth"), fixture.window.width());
    result.stats.insert(QStringLiteral("windowLogicalHeight"), fixture.window.height());
    result.stats.insert(QStringLiteral("windowPhysicalWidth"),
                        qRound(fixture.window.width() * fixture.window.devicePixelRatio()));
    result.stats.insert(QStringLiteral("windowPhysicalHeight"),
                        qRound(fixture.window.height() * fixture.window.devicePixelRatio()));
    result.stats.insert(QStringLiteral("ok"), result.ok);
    result.stats.insert(QStringLiteral("error"), result.error);
    result.stats.insert(QStringLiteral("exitCode"), result.ok ? 0 : 1);
    writeJson(outPath, result.stats);
    QFile raw(outPath);
    const bool committed = raw.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(raw.readAll()).isObject();
    raw.close();
    result.stats = ownedObservation(result.stats);
    std::fprintf(stderr, "[e2e] finish: raw json committed=%d before unload\n", committed);
    std::fflush(stderr);
    QString unloadError;
    const auto started = QDateTime::currentMSecsSinceEpoch();
    const bool unloaded = fixture.unload(&unloadError);
    const int code = result.ok && committed && unloaded ? 0 : 1;
    QVariantMap outcome;
    outcome.insert(QStringLiteral("unloadOk"), unloaded);
    outcome.insert(QStringLiteral("unloadError"), unloadError);
    outcome.insert(QStringLiteral("unloadElapsedMs"), QDateTime::currentMSecsSinceEpoch() - started);
    outcome.insert(QStringLiteral("rawResultCommitted"), committed);
    outcome.insert(QStringLiteral("finalExitCode"), code);
    writeJson(QFileInfo(outPath).absolutePath() + QLatin1Char('/') + unloadRecord, outcome);
    std::fprintf(stderr, "[e2e] finish: unload recorded, exit=%d\n", code);
    std::fflush(stderr);
    return code;
}

int runPublisher()
{
    Result result;
    Fixture fixture;
    QString error;
    result.require(fixture.load(&error), error);
    if (!result.ok) return finish(result, env("ZDS_E2E_OUT"), fixture);
    fixture.window.show();
    pump(300);
    auto *controller = fixture.controller;
    const auto profile = env("ZDS_E2E_PROFILE", QStringLiteral("720p30"));
    const auto codec = env("ZDS_E2E_CODEC", QStringLiteral("bgra-tiles"));
    const auto tag = env("ZDS_E2E_TAG", QStringLiteral("A"));
    const auto server = env("ZDS_E2E_SERVER", QStringLiteral("127.0.0.1:18100"));
    const int expectedViewers = envInt("ZDS_E2E_EXPECT_VIEWERS", 1);
    const int durationMs = envInt("ZDS_E2E_DURATION_MS", 20000);
    const int switchAtMs = envInt("ZDS_E2E_SWITCH_AT_MS", -1);
    const auto switchProfile = env("ZDS_E2E_SWITCH_PROFILE");
    const auto switchTag = env("ZDS_E2E_SWITCH_TAG");
    // Static weak-network marker challenge: one deliberate repaint of the same
    // frozen source after the fault is cleared. A new colour can only appear if
    // genuinely new content was captured, encoded and delivered; a cached
    // keyframe refresh would keep the old colour.
    const int markAtMs = envInt("ZDS_E2E_MARK_AT_MS", -1);
    const auto markTag = env("ZDS_E2E_MARK_TAG");
    const bool lightSamples = envInt("ZDS_E2E_SAMPLE_LIGHT") != 0;
    const auto appendPublisherSample = [lightSamples](QVariantList &list, const QVariant &entry) {
        if (!lightSamples || list.size() < 3) list.append(entry);
        else list[list.size() - 1] = entry;
    };
    const auto roomFile = env("ZDS_E2E_ROOMCODE_FILE");

    const auto sourceKind = env("ZDS_E2E_SOURCE_KIND", QStringLiteral("window"));
    TagWidget source(tagColor(tag));
    const bool staticSource = envInt("ZDS_E2E_STATIC") != 0;
    if (!staticSource && envInt("ZDS_E2E_LATENCY")) source.enableLatencyBar();
    if (envInt("ZDS_E2E_GRID")) source.enableGrid();
    if (sourceKind == QStringLiteral("screen")) {
        // Test-only: paint a dedicated synthetic source and exercise the real
        // monitor capture path. The frameless top-most window covers the whole
        // screen, so the captured pixels contain only synthetic test content.
        source.setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        if (const auto *screen = QGuiApplication::primaryScreen())
            source.setGeometry(screen->geometry());
        source.showFullScreen();
    } else {
        // Place the fixture inside the logical screen area so DPI scaling runs
        // (QT_SCALE_FACTOR) do not push the only source off-screen.
        const auto *primary = QGuiApplication::primaryScreen();
        const QRect available = primary ? primary->availableGeometry()
                                        : QRect(0, 0, 1920, 1200);
        const int fixtureWidth = 480;
        const int fixtureHeight = 270;
        source.resize(fixtureWidth, fixtureHeight);
        source.move(qMax(0, available.right() - fixtureWidth - 40),
                    qMax(0, available.bottom() - fixtureHeight - 80));
        source.show();
    }
    // A static source must not be restarted: the earlier branch froze the QTimer
    // and then immediately called start(), so the "static" case kept repainting.
    if (!staticSource) source.start();
    else source.freeze();
    pump(400);
    source.raise();
    source.activateWindow();

    QMetaObject::invokeMethod(controller, "refreshSources");
    if (sourceKind == QStringLiteral("screen")) {
        result.require(
            waitFor(
                [&] {
                    for (const auto &entry : controller->property("sources").toList())
                        if (entry.toMap().value(QStringLiteral("kind")).toString()
                            == QStringLiteral("screen"))
                            return true;
                    return false;
                },
                30000),
            QStringLiteral("来源枚举超时"));
    } else {
        result.require(
            waitFor(
                [&] {
                    for (const auto &entry : controller->property("sources").toList()) {
                        const auto map = entry.toMap();
                        if (map.value(QStringLiteral("description")).toString().contains(source.title()))
                            return true;
                    }
                    return false;
                },
                30000),
            QStringLiteral("来源枚举超时"));
    }
    if (!result.ok) return finish(result, env("ZDS_E2E_OUT"), fixture);
    QString sourceValue;
    for (const auto &entry : controller->property("sources").toList()) {
        const auto map = entry.toMap();
        const auto kind = map.value(QStringLiteral("kind")).toString();
        if (sourceKind == QStringLiteral("screen") && kind == QStringLiteral("screen")) {
            sourceValue = map.value(QStringLiteral("value")).toString();
            break;
        }
        if (sourceKind != QStringLiteral("screen") && kind == QStringLiteral("window")
            && map.value(QStringLiteral("description")).toString().contains(source.title()))
            sourceValue = map.value(QStringLiteral("value")).toString();
    }
    result.require(!sourceValue.isEmpty(),
                   sourceKind == QStringLiteral("screen") ? QStringLiteral("屏幕来源不可捕获")
                                                          : QStringLiteral("合成来源窗口不可捕获"));
    if (!result.ok) return finish(result, env("ZDS_E2E_OUT"), fixture);

    controller->setProperty("serverAddress", server);
    controller->setProperty("profile", profile);
    controller->setProperty("codec", codec);
    controller->setProperty("sourceValue", sourceValue);
    QMetaObject::invokeMethod(controller, "createRoom");
    result.require(waitFor([&] { return controller->property("roomCode").toString().length() == 8; },
                           20000),
                   QStringLiteral("创建房间超时：%1").arg(controller->property("statusText").toString()));
    if (!result.ok) return finish(result, env("ZDS_E2E_OUT"), fixture);
    writeTrimmed(roomFile, controller->property("roomCode").toString());
    result.stats.insert(QStringLiteral("roomCodeEpochMs"),
                        double(QDateTime::currentMSecsSinceEpoch()));
    result.require(waitFor([&] { return controller->property("publishing").toBool(); }, 15000),
                   QStringLiteral("未进入共享状态：%1").arg(controller->property("statusText").toString()));
    // A static source legitimately presents very few local-loopback frames; the
    // first-frame requirement must not turn into a repaint requirement.
    const int firstLoopbackFrames = staticSource ? 1 : 5;
    result.require(waitFor([&] { return controller->property("decodedFrames").toInt() >= firstLoopbackFrames; }, 25000),
                   QStringLiteral("本地回环首帧超时：%1").arg(controller->property("statusText").toString()));
    result.require(waitFor([&] { return controller->property("viewerCount").toInt() >= expectedViewers; },
                           30000),
                   QStringLiteral("观看者未全部加入：%1").arg(controller->property("statusText").toString()));

    QElapsedTimer runTimer;
    runTimer.start();
    int maxViewers = controller->property("viewerCount").toInt();
    bool switched = false;
    bool marked = false;
    TagWidget switchSource(tagColor(switchTag));
    if (envInt("ZDS_E2E_LATENCY")) switchSource.enableLatencyBar();
    if (envInt("ZDS_E2E_GRID")) switchSource.enableGrid();
    switchSource.resize(480, 270);
    switchSource.move(1350, 500);
    QVariantList publisherSamples;
    const auto queueLog = env("ZDS_E2E_QUEUE_LOG");
    const auto queueRole = env("ZDS_E2E_QUEUE_ROLE", QStringLiteral("publisher"));
    const int protectSourceAtMs = envInt("ZDS_E2E_PROTECT_SOURCE_AT_MS", -1);
    const int closeSourceAtMs = envInt("ZDS_E2E_CLOSE_SOURCE_AT_MS", -1);
    bool sourceProtected = false;
    bool sourceClosed = false;
    qint64 nextPublisherSample = 0;
    while (result.ok) {
        pump(200);
        const int elapsed = int(runTimer.elapsed());
        maxViewers = qMax(maxViewers, controller->property("viewerCount").toInt());
        if (elapsed >= nextPublisherSample) {
            nextPublisherSample = elapsed + 1000;
            const auto snapshot = controllerStats(controller);
            QVariantMap entry;
            entry.insert(QStringLiteral("elapsedMs"), elapsed);
            entry.insert(QStringLiteral("epochMs"),
                         double(QDateTime::currentMSecsSinceEpoch()));
            entry.insert(QStringLiteral("viewerCount"),
                         controller->property("viewerCount").toInt());
            entry.insert(QStringLiteral("phase"), controller->property("phase"));
            entry.insert(QStringLiteral("pipeline"), snapshot);
            entry.insert(QStringLiteral("uploadPackets"),
                         snapshot.value(QStringLiteral("uploadPackets")).toString());
            entry.insert(QStringLiteral("datagramsSent"),
                         snapshot.value(QStringLiteral("datagramsSent")).toString());
            entry.insert(QStringLiteral("publishRateBytesPerSecond"),
                         snapshot.value(QStringLiteral("publishRateBytesPerSecond")).toString());
            entry.insert(QStringLiteral("uploadDropped"),
                         snapshot.value(QStringLiteral("uploadDropped")).toString());
            entry.insert(QStringLiteral("outboundQueued"),
                         snapshot.value(QStringLiteral("outboundQueued")).toInt());
            entry.insert(QStringLiteral("outboundBytes"),
                         snapshot.value(QStringLiteral("outboundBytes")).toLongLong());
            appendPublisherSample(publisherSamples, ownedObservation(entry));
            appendObservationLine(queueLog,
                                  QVariantMap{{QStringLiteral("epochMs"),
                                               double(QDateTime::currentMSecsSinceEpoch())},
                                              {QStringLiteral("role"), queueRole},
                                              {QStringLiteral("elapsedMs"), elapsed},
                                              {QStringLiteral("pipeline"), snapshot}});
        }
        if (!sourceProtected && protectSourceAtMs >= 0 && elapsed >= protectSourceAtMs) {
            sourceProtected = true;
#ifdef Q_OS_WIN
            // WDA_EXCLUDEFROMCAPTURE: the window stays visible to the user but
            // is excluded from any screen/window capture. This is the same
            // mechanism used by protected content; it is a reproducible
            // substitute for a secure-desktop/protected-window capture denial.
            const bool protectedOk =
                SetWindowDisplayAffinity(reinterpret_cast<HWND>(source.nativeHandle()), 0x11u) != 0;
            result.stats.insert(QStringLiteral("protectSourceOk"), protectedOk);
#endif
            result.stats.insert(QStringLiteral("protectSourceEpochMs"),
                                double(QDateTime::currentMSecsSinceEpoch()));
            result.stats.insert(QStringLiteral("protectSourceElapsedMs"), elapsed);
        }
        if (!sourceClosed && closeSourceAtMs >= 0 && elapsed >= closeSourceAtMs) {
            sourceClosed = true;
            result.stats.insert(QStringLiteral("sourceCloseEpochMs"),
                                double(QDateTime::currentMSecsSinceEpoch()));
            result.stats.insert(QStringLiteral("sourceCloseElapsedMs"), elapsed);
            // Reproducible substitute for a shared monitor/window disappearing
            // (unplug or owner exit): the only source is closed and the product
            // must report a failure instead of silently switching to another one.
            source.close();
        }
        if (!marked && markAtMs >= 0 && elapsed >= markAtMs && !markTag.isEmpty()) {
            marked = true;
            source.setTagColour(tagColor(markTag));
            result.stats.insert(QStringLiteral("markEpochMs"),
                                double(QDateTime::currentMSecsSinceEpoch()));
            result.stats.insert(QStringLiteral("markElapsedMs"), elapsed);
            result.stats.insert(QStringLiteral("markTag"), markTag);
            result.stats.insert(QStringLiteral("markPaints"), source.paints());
        }
        if (!switched && switchAtMs >= 0 && elapsed >= switchAtMs) {
            switched = true;
            if (!switchProfile.isEmpty()) {
                controller->setProperty("profile", switchProfile);
                pump(1500);
            }
            if (!switchTag.isEmpty()) {
                switchSource.show();
                switchSource.start();
                pump(400);
                switchSource.raise();
                switchSource.activateWindow();
                QString secondValue;
                QMetaObject::invokeMethod(controller, "refreshSources");
                waitFor([&] {
                    for (const auto &entry : controller->property("sources").toList()) {
                        const auto map = entry.toMap();
                        if (map.value(QStringLiteral("description")).toString().contains(
                                switchSource.title()))
                            return true;
                    }
                    return false;
                }, 10000);
                for (const auto &entry : controller->property("sources").toList()) {
                    const auto map = entry.toMap();
                    if (map.value(QStringLiteral("description")).toString().contains(
                            switchSource.title()))
                        secondValue = map.value(QStringLiteral("value")).toString();
                }
                result.require(!secondValue.isEmpty(), QStringLiteral("切换来源未出现在列表中"));
                controller->setProperty("sourceValue", secondValue);
            }
            result.stats.insert(QStringLiteral("switched"), true);
        }
        if (elapsed >= durationMs) break;
    }
    result.require(controller->property("publishing").toBool(),
                   QStringLiteral("共享意外结束：%1").arg(controller->property("statusText").toString()));
    result.require(maxViewers >= expectedViewers,
                   QStringLiteral("观看者数量不足：峰值 %1，期望 %2")
                       .arg(maxViewers)
                       .arg(expectedViewers));
    const auto stats = controllerStats(controller);
    result.stats.insert(QStringLiteral("stats"), stats);
    result.stats.insert(QStringLiteral("publisherSamples"), publisherSamples);
    result.stats.insert(QStringLiteral("profile"), controller->property("profile").toString());
    result.stats.insert(QStringLiteral("codec"), codec);
    result.stats.insert(QStringLiteral("viewerCount"), controller->property("viewerCount").toInt());
    result.stats.insert(QStringLiteral("maxViewers"), maxViewers);
    result.stats.insert(QStringLiteral("sourceStatic"), staticSource);
    result.stats.insert(QStringLiteral("sourcePaints"), source.paints());
    result.stats.insert(QStringLiteral("sourceTicks"), source.ticks());
    result.stats.insert(QStringLiteral("sourceCloseEvents"), source.closeEvents());
    result.stats.insert(QStringLiteral("sourceHwnd"),
                        QString::number(quint64(source.nativeHandle()), 16));
    result.stats.insert(QStringLiteral("sourceProcessId"),
                        double(QCoreApplication::applicationPid()));
    result.stats.insert(QStringLiteral("sourceCapturedFrames"),
                        controller->property("capturedFrames").toInt());
    result.require(stats.value(QStringLiteral("uploadPackets")).toString().toULongLong() > 0,
                   QStringLiteral("发布端没有上传编码包"));
    result.require(stats.value(QStringLiteral("datagramsSent")).toString().toULongLong() > 0,
                   QStringLiteral("发布端没有发送 UDP 报文"));
    result.require(stats.value(QStringLiteral("completePackets")).toString().toULongLong() == 0,
                   QStringLiteral("发布端不应重组收到的视频"));
    const auto preview = previewImage(fixture.page);
    const auto shot = env("ZDS_E2E_SCREENSHOT");
    if (!shot.isEmpty()) result.stats.insert(QStringLiteral("screenshot"), preview.save(shot));
    std::fprintf(stderr, "[e2e] publisher: stopSharing\n");
    std::fflush(stderr);
    const qint64 stopRequestedEpochMs = QDateTime::currentMSecsSinceEpoch();
    QMetaObject::invokeMethod(controller, "stopSharing");
    result.require(waitFor([&] { return controller->property("state").toString() == QStringLiteral("idle"); },
                           15000),
                   QStringLiteral("停止共享未完成：%1").arg(controller->property("statusText").toString()));
    result.require(!controller->property("publishing").toBool(), QStringLiteral("停止后仍处于共享状态"));
    result.stats.insert(QStringLiteral("stopRequestedEpochMs"), double(stopRequestedEpochMs));
    result.stats.insert(QStringLiteral("stopCompletedEpochMs"),
                        double(QDateTime::currentMSecsSinceEpoch()));
    // Post-stop snapshot: closes the "no per-client post-stop queue counters"
    // evidence gap without changing the pre-stop statistics.
    pump(300);
    const auto stoppedStats = controllerStats(controller);
    result.stats.insert(QStringLiteral("statsAfterStop"), stoppedStats);
    result.require(stoppedStats.value(QStringLiteral("outboundBytes")).toString()
                       == QStringLiteral("0"),
                   QStringLiteral("停止后发布队列仍有待发字节"));
    result.require(stoppedStats.value(QStringLiteral("outboundQueued")).toInt() == 0,
                   QStringLiteral("停止后发布队列仍有待发包"));
    source.stop();
    switchSource.stop();
    std::fprintf(stderr, "[e2e] publisher: done, finishing\n");
    std::fflush(stderr);
    return finish(result, env("ZDS_E2E_OUT"), fixture);
}

int runViewer()
{
    Result result;
    Fixture fixture;
    QString error;
    result.require(fixture.load(&error), error);
    if (!result.ok) return finish(result, env("ZDS_E2E_OUT"), fixture);
    fixture.window.show();
    pump(300);
    auto *controller = fixture.controller;
    const auto server = env("ZDS_E2E_SERVER", QStringLiteral("127.0.0.1:18100"));
    const auto roomFile = env("ZDS_E2E_ROOMCODE_FILE");
    const auto expectTag = env("ZDS_E2E_EXPECT_TAG");
    const auto expectEndTag = env("ZDS_E2E_EXPECT_END_TAG");
    const int expectWidth = envInt("ZDS_E2E_EXPECT_WIDTH", 1280);
    const int expectHeight = envInt("ZDS_E2E_EXPECT_HEIGHT", 720);
    const int durationMs = envInt("ZDS_E2E_DURATION_MS", 20000);
    const int lateJoinMs = envInt("ZDS_E2E_LATE_JOIN_MS", 0);
    const int afterSwitchMs = envInt("ZDS_E2E_AFTER_SWITCH_MS", 0);
    const int expectEndWidth = envInt("ZDS_E2E_EXPECT_END_WIDTH", 0);
    const int expectEndHeight = envInt("ZDS_E2E_EXPECT_END_HEIGHT", 0);
    const int sampleMs = envInt("ZDS_E2E_SAMPLE_MS", 0);
    const bool latencyProbe = envInt("ZDS_E2E_LATENCY") != 0;

    controller->setProperty("serverAddress", server);
    if (lateJoinMs > 0) {
        result.require(waitFor([&] { return readTrimmed(roomFile).length() == 8; }, 45000),
                       QStringLiteral("等待房间号超时"));
        if (!result.ok) return finish(result, env("ZDS_E2E_OUT"), fixture);
        pump(lateJoinMs);
    }
    result.require(waitFor([&] { return readTrimmed(roomFile).length() == 8; }, 60000),
                   QStringLiteral("等待房间号超时"));
    if (!result.ok) return finish(result, env("ZDS_E2E_OUT"), fixture);
    // The deliberate late-join delay is not part of the join-to-first-frame
    // measurement; start the clock at the actual join request.
    const qint64 joinStartEpochMs = QDateTime::currentMSecsSinceEpoch();
    controller->setProperty("joinCode", readTrimmed(roomFile));
    QMetaObject::invokeMethod(controller, "joinRoom");
    // Retain the entire first-frame pipeline even when no frame ever decodes.
    QVariantList startupSamples;
    QTimer startupTimer;
    const auto startupSample = [&] {
        QVariantMap observation;
        QMetaObject::invokeMethod(controller, "recoveryObservation", Q_RETURN_ARG(QVariantMap, observation));
        startupSamples.append(ownedObservation(QVariantMap{{QStringLiteral("epochMs"), double(QDateTime::currentMSecsSinceEpoch())},
            {QStringLiteral("recovery"), observation}, {QStringLiteral("pipeline"), controllerStats(controller)},
            {QStringLiteral("phase"), controller->property("phase")}}));
        result.stats.insert(QStringLiteral("startupSamples"), startupSamples);
    };
    QObject::connect(&startupTimer, &QTimer::timeout, controller, startupSample);
    startupTimer.start(500);
    startupSample();
    const auto expectJoinFail = env("ZDS_E2E_EXPECT_JOIN_FAIL");
    if (!expectJoinFail.isEmpty()) {
        QElapsedTimer failureTimer;
        failureTimer.start();
        const bool rejected = waitFor(
            [&] {
                const auto error = controller->property("errorText").toString();
                return !controller->property("watching").toBool() && !error.isEmpty();
            },
            20000);
        const auto stats = controllerStats(controller);
        result.stats.insert(QStringLiteral("stats"), stats);
        result.stats.insert(QStringLiteral("statusText"), controller->property("statusText").toString());
        result.stats.insert(QStringLiteral("errorText"), controller->property("errorText").toString());
        result.stats.insert(QStringLiteral("phase"), controller->property("phase").toString());
        result.stats.insert(QStringLiteral("joinFailureWaitMs"), failureTimer.elapsed());
        result.require(rejected, QStringLiteral("预期入房被拒绝，但没有可见失败"));
        result.require(!controller->property("watching").toBool(),
                       QStringLiteral("入房被拒绝后仍处于观看状态"));
        const auto text = controller->property("errorText").toString()
            + controller->property("statusText").toString();
        result.require(text.contains(expectJoinFail),
                       QStringLiteral("拒绝原因缺少 %1：%2").arg(expectJoinFail, text));
        return finish(result, env("ZDS_E2E_OUT"), fixture);
    }
    result.require(waitFor([&] { return controller->property("watching").toBool(); }, 30000),
                   QStringLiteral("观看端未就绪：%1").arg(controller->property("statusText").toString()));
    result.stats.insert(QStringLiteral("readyEpochMs"),
                        double(QDateTime::currentMSecsSinceEpoch()));
    result.stats.insert(QStringLiteral("readyDelayMs"),
                        double(QDateTime::currentMSecsSinceEpoch() - joinStartEpochMs));
    result.stats.insert(QStringLiteral("statsAfterReady"), controllerStats(controller));
    if (envInt("ZDS_E2E_EXPECT_NO_MEDIA")) {
        QElapsedTimer elapsed; elapsed.start();
        result.require(waitFor([&] { return !controller->property("watching").toBool(); },12000),
                       QStringLiteral("无媒体观看未按首帧期限结束"));
        const auto stats = controllerStats(controller);
        result.stats.insert(QStringLiteral("stats"), stats);
        result.stats.insert(QStringLiteral("timeoutElapsedMs"), elapsed.elapsed());
        result.require(elapsed.elapsed() >= 9000 && elapsed.elapsed() < 11500,
                       QStringLiteral("首帧期限发生改变"));
        result.require(controller->property("connected").toBool(),QStringLiteral("控制连接已断开"));
        result.require(controller->property("decodedFrames").toInt()==0,QStringLiteral("无媒体却产生画面"));
        result.require(controller->property("statusText").toString().contains(QStringLiteral("10 秒")),
                       QStringLiteral("缺少首帧超时原因"));
        return finish(result,env("ZDS_E2E_OUT"),fixture);
    }
    result.require(waitFor([&] { return controller->property("decodedFrames").toInt() >= 1; }, 30000),
                   QStringLiteral("首帧超时：%1").arg(controller->property("statusText").toString()));
    result.stats.insert(QStringLiteral("firstFrameEpochMs"),
                        double(QDateTime::currentMSecsSinceEpoch()));
    result.stats.insert(QStringLiteral("firstFrameDelayMs"),
                        double(QDateTime::currentMSecsSinceEpoch() - joinStartEpochMs));
    const bool staticViewer = envInt("ZDS_E2E_STATIC") != 0;
    // Resource-control mode: keep only a rolling window of three full samples so
    // a long run does not accumulate ten megabytes of QVariantMaps inside the
    // harness itself. The product path is unchanged; only the observer's own
    // history is bounded, which is what the RSS control experiment measures.
    const bool lightSamples = envInt("ZDS_E2E_SAMPLE_LIGHT") != 0;
    const auto queueLog = env("ZDS_E2E_QUEUE_LOG");
    const auto queueRole = env("ZDS_E2E_QUEUE_ROLE", QStringLiteral("viewer"));
    const auto appendSample = [lightSamples](QVariantList &list, const QVariant &entry) {
        if (!lightSamples || list.size() < 3) list.append(entry);
        else list[list.size() - 1] = entry;
    };
    const int firstFramesTarget = staticViewer ? 1 : 10;
    const bool firstFrames = waitFor([&] { return controller->property("decodedFrames").toInt() >= firstFramesTarget; }, 30000);
    result.stats.insert(QStringLiteral("statsAfterFirstFrameWait"), controllerStats(controller));
    result.require(firstFrames,
                   QStringLiteral("首帧超时：%1").arg(controller->property("statusText").toString()));
    if (!result.ok) {
        // Preserve the full timeout diagnostics, including partial reassembly
        // and rejected/expired fragments, rather than only the early snapshot.
        result.stats.insert(QStringLiteral("stats"), controllerStats(controller));
        result.stats.insert(QStringLiteral("statusText"), controller->property("statusText").toString());
        result.stats.insert(QStringLiteral("errorText"), controller->property("errorText").toString());
        result.stats.insert(QStringLiteral("phase"), controller->property("phase").toString());
        result.stats.insert(QStringLiteral("firstFrameWaitTimeoutMs"), 30000);
        return finish(result, env("ZDS_E2E_OUT"), fixture);
    }
    QElapsedTimer sampleTimer;
    startupTimer.stop();
    sampleTimer.start();
    QVariantList frameSamples;
    QVariantList latencySamples;
    bool staticPageCaptured = false;
    bool staticPage3sCaptured = false;
    bool staticPage10sCaptured = false;
    bool rateLimitedSeen = false;
    QVariantMap feedbackProbe;
    // The delivered package may predate the feedback observation hook (used for
    // the A/B reproduction of the old cadence); the visible page text is always
    // available and is the authoritative "what the user sees" evidence.
    const bool feedbackSupported = QMetaObject::invokeMethod(
        controller, "feedbackObservation", Q_RETURN_ARG(QVariantMap, feedbackProbe));
    const auto noteVisibleErrors = [&rateLimitedSeen](const QString &text) {
        if (text.contains(QStringLiteral("RATE_LIMITED"))
            || text.contains(QStringLiteral("接收反馈上报过于频繁")))
            rateLimitedSeen = true;
    };
    const auto sample = [&] {
        QVariantMap entry;
        entry.insert(QStringLiteral("epochMs"),
                     double(QDateTime::currentMSecsSinceEpoch()));
        entry.insert(QStringLiteral("elapsedMs"), sampleTimer.elapsed());
        entry.insert(QStringLiteral("frames"), controller->property("decodedFrames").toInt());
        entry.insert(QStringLiteral("connected"), controller->property("connected").toBool());
        entry.insert(QStringLiteral("watching"), controller->property("watching").toBool());
        entry.insert(QStringLiteral("status"), controller->property("statusText").toString());
        // The controller error and the visible page labels are recorded
        // separately: statusText alone hid an earlier RATE_LIMITED that was
        // rendered as the details line.
        entry.insert(QStringLiteral("errorText"), controller->property("errorText").toString());
        entry.insert(QStringLiteral("details"), controller->property("details").toString());
        if (auto *error = fixture.page->findChild<QQuickItem *>(QStringLiteral("desktopShareError"))) {
            entry.insert(QStringLiteral("pageErrorText"), error->property("text").toString());
            entry.insert(QStringLiteral("pageErrorVisible"), error->isVisible());
        }
        if (auto *details = fixture.page->findChild<QQuickItem *>(QStringLiteral("desktopShareDetails"))) {
            entry.insert(QStringLiteral("pageDetailsText"), details->property("text").toString());
            entry.insert(QStringLiteral("pageDetailsVisible"), details->isVisible());
        }
        if (auto *status = fixture.page->findChild<QQuickItem *>(QStringLiteral("desktopShareStatus"))) {
            entry.insert(QStringLiteral("pageStatusText"), status->property("text").toString());
        }
        noteVisibleErrors(controller->property("statusText").toString());
        noteVisibleErrors(controller->property("errorText").toString());
        noteVisibleErrors(controller->property("details").toString());
        noteVisibleErrors(entry.value(QStringLiteral("pageDetailsText")).toString());
        noteVisibleErrors(entry.value(QStringLiteral("pageErrorText")).toString());
        noteVisibleErrors(entry.value(QStringLiteral("pageStatusText")).toString());
        QVariantMap feedback;
        QMetaObject::invokeMethod(controller, "feedbackObservation", Q_RETURN_ARG(QVariantMap, feedback));
        entry.insert(QStringLiteral("feedback"), feedback);
        QVariantMap recovery;
        QMetaObject::invokeMethod(controller, "recoveryObservation", Q_RETURN_ARG(QVariantMap, recovery));
        entry.insert(QStringLiteral("recovery"), recovery);
        entry.insert(QStringLiteral("phase"), controller->property("phase"));
        entry.insert(QStringLiteral("pipeline"), controllerStats(controller));
        if (auto *badge = fixture.page->findChild<QQuickItem *>(QStringLiteral("desktopShareStatusBadge"))) {
            entry.insert(QStringLiteral("pageBadgeText"), badge->property("text").toString());
            entry.insert(QStringLiteral("pageBadgeVisible"), badge->isVisible());
        }
        if (staticViewer && !staticPageCaptured && sampleTimer.elapsed() >= 12000) {
            staticPageCaptured = true;
            const auto pagePath = env("ZDS_E2E_SCREENSHOT") + QStringLiteral(".after12s-page.png");
            result.stats.insert(QStringLiteral("staticPageAfter12s"), fixture.window.grabWindow().save(pagePath));
            result.stats.insert(QStringLiteral("staticPageAfter12sPhase"), controller->property("phase"));
        }
        if (staticViewer && !staticPage3sCaptured && sampleTimer.elapsed() >= 3000) {
            staticPage3sCaptured = true;
            const auto pagePath = env("ZDS_E2E_SCREENSHOT") + QStringLiteral(".after3s-page.png");
            result.stats.insert(QStringLiteral("staticPageAfter3s"), fixture.window.grabWindow().save(pagePath));
            result.stats.insert(QStringLiteral("staticPageAfter3sPhase"), controller->property("phase"));
            result.stats.insert(QStringLiteral("staticPageAfter3sErrorText"), controller->property("errorText").toString());
            result.stats.insert(QStringLiteral("staticPageAfter3sDetails"), controller->property("details").toString());
        }
        if (staticViewer && !staticPage10sCaptured && sampleTimer.elapsed() >= 10000) {
            staticPage10sCaptured = true;
            const auto pagePath = env("ZDS_E2E_SCREENSHOT") + QStringLiteral(".after10s-page.png");
            result.stats.insert(QStringLiteral("staticPageAfter10s"), fixture.window.grabWindow().save(pagePath));
            result.stats.insert(QStringLiteral("staticPageAfter10sPhase"), controller->property("phase"));
            result.stats.insert(QStringLiteral("staticPageAfter10sErrorText"), controller->property("errorText").toString());
            result.stats.insert(QStringLiteral("staticPageAfter10sDetails"), controller->property("details").toString());
        }
        if (latencyProbe) {
            const auto image = previewImage(fixture.page);
            qint64 decoded = 0;
            if (decodeTimeCode(image, &decoded)) {
                const qint64 localMs = QDateTime::currentMSecsSinceEpoch();
                qint64 latencyMs = 0;
                if (timeCodeLatencyMs(localMs, decoded, &latencyMs)) {
                    QVariantMap latency;
                    latency.insert(QStringLiteral("epochMs"), double(localMs));
                    latency.insert(QStringLiteral("elapsedMs"), sampleTimer.elapsed());
                    latency.insert(QStringLiteral("frames"), entry.value(QStringLiteral("frames")));
                    latency.insert(QStringLiteral("publisherEpochMs"), double(decoded));
                    latency.insert(QStringLiteral("latencyMs"), double(latencyMs));
                    latencySamples.append(latency);
                    entry.insert(QStringLiteral("latencyMs"), double(latencyMs));
                }
            }
        }
        appendSample(frameSamples, ownedObservation(entry));
        appendObservationLine(queueLog,
                              QVariantMap{{QStringLiteral("epochMs"),
                                           double(QDateTime::currentMSecsSinceEpoch())},
                                          {QStringLiteral("role"), queueRole},
                                          {QStringLiteral("elapsedMs"), sampleTimer.elapsed()},
                                          {QStringLiteral("pipeline"),
                                           entry.value(QStringLiteral("pipeline"))}});
    };
    sample();
    const int stallMs = envInt("ZDS_E2E_STALL_MS");
    const int stallAtMs = envInt("ZDS_E2E_STALL_AT_MS");
    if (stallMs > 0) {
        if (stallAtMs > 0) {
            waitFor([&] { return sampleTimer.elapsed() >= stallAtMs; }, stallAtMs + 5000);
            sample();
        }
        const int before = controller->property("decodedFrames").toInt();
        result.stats.insert(QStringLiteral("stallStartEpochMs"),
                            double(QDateTime::currentMSecsSinceEpoch()));
        result.stats.insert(QStringLiteral("stallStartFrames"), before);
        QThread::msleep(stallMs);
        result.stats.insert(QStringLiteral("stallEndEpochMs"),
                            double(QDateTime::currentMSecsSinceEpoch()));
        result.stats.insert(QStringLiteral("stallEndFrames"),
                            controller->property("decodedFrames").toInt());
        sample();
        result.require(waitFor([&] { return controller->property("decodedFrames").toInt() > before + 10; },5000),
                       QStringLiteral("单个慢观看者未恢复显示"));
        sample();
        const auto stalled = controllerStats(controller);
        result.stats.insert(QStringLiteral("statsAfterStall"), stalled);
        result.require(stalled.value(QStringLiteral("inboundDroppedBudget")).toString().toULongLong()>0,
                       QStringLiteral("慢观看者没有触发 GUI 邮箱丢包"));
    }
    result.require(controller->property("frameWidth").toInt() == expectWidth
                       && controller->property("frameHeight").toInt() == expectHeight,
                   QStringLiteral("画面尺寸 %1×%2 与期望 %3×%4 不符")
                       .arg(controller->property("frameWidth").toInt())
                       .arg(controller->property("frameHeight").toInt())
                       .arg(expectWidth)
                       .arg(expectHeight));
    QImage picture = previewImage(fixture.page);
    const double fraction = expectTag.isEmpty() ? 1.0 : colourFraction(picture, tagColor(expectTag));
    result.stats.insert(QStringLiteral("initialColourFraction"), fraction);
    result.require(expectTag.isEmpty() || fraction > 0.05,
                   QStringLiteral("预览中未找到来源颜色（比例 %1）").arg(fraction));
    const auto shot = env("ZDS_E2E_SCREENSHOT");
    if (!shot.isEmpty()) {
        const auto snapshot = previewImage(fixture.page);
        result.stats.insert(QStringLiteral("screenshot"), snapshot.save(shot));
    }

    if (afterSwitchMs > 0) {
        pump(afterSwitchMs);
        bool endMatched = true;
        if (!expectEndTag.isEmpty()) {
            endMatched = waitFor(
                [&] {
                    const auto image = previewImage(fixture.page);
                    return colourFraction(image, tagColor(expectEndTag)) > 0.05;
                },
                30000);
        }
        result.require(endMatched, QStringLiteral("切换来源后未显示新的来源画面"));
        if (expectEndWidth > 0) {
            const bool sized = waitFor(
                [&] {
                    return controller->property("frameWidth").toInt() == expectEndWidth
                        && controller->property("frameHeight").toInt() == expectEndHeight;
                },
                30000);
            result.require(sized, QStringLiteral("切换档位后画面尺寸未变为 %1×%2")
                                      .arg(expectEndWidth)
                                      .arg(expectEndHeight));
            result.stats.insert(QStringLiteral("endFrameWidth"),
                                controller->property("frameWidth").toInt());
            result.stats.insert(QStringLiteral("endFrameHeight"),
                                controller->property("frameHeight").toInt());
        }
        const auto endImage = previewImage(fixture.page);
        result.stats.insert(QStringLiteral("endColourFraction"),
                            expectEndTag.isEmpty()
                                ? 0.0
                                : colourFraction(endImage, tagColor(expectEndTag)));
        result.stats.insert(QStringLiteral("switchedFrames"), controller->property("decodedFrames").toInt());
    }

    const int startFrames = controller->property("decodedFrames").toInt();
    if (sampleMs > 0) {
        QElapsedTimer observation;
        observation.start();
        while (observation.elapsed() < durationMs && result.ok) {
            const int step = qMin(sampleMs, int(durationMs - observation.elapsed()));
            pump(qMax(1, step));
            sample();
        }
    } else {
        pump(durationMs);
    }
    sample();
    const int endFrames = controller->property("decodedFrames").toInt();
    result.stats.insert(QStringLiteral("frameSamples"), frameSamples);
    result.stats.insert(QStringLiteral("latencySamples"), latencySamples);
    result.stats.insert(QStringLiteral("latencySamplesCount"), latencySamples.size());
    result.stats.insert(QStringLiteral("startFrames"), startFrames);
    result.stats.insert(QStringLiteral("endFrames"), endFrames);
    // The session must still be alive at the end of the observation window; a
    // server failure has to end the viewing instead of silently freezing on an
    // old picture.
    result.require(controller->property("connected").toBool()
                       && controller->property("watching").toBool(),
                   QStringLiteral("观看会话在结束前已中断：%1")
                       .arg(controller->property("statusText").toString()));
    if (staticViewer) {
        // A static source may present exactly one image for the whole window;
        // that is valid as long as the session stays alive and the first frame
        // was decoded. Record whether any additional frame arrived.
        result.require(endFrames >= 1, QStringLiteral("静态来源没有解码首帧"));
        result.stats.insert(QStringLiteral("staticFramesUnchanged"), endFrames == startFrames);
        result.stats.insert(QStringLiteral("phaseAtEnd"), controller->property("phase").toString());
        result.stats.insert(QStringLiteral("statusAtEnd"), controller->property("statusText").toString());
    } else {
        result.require(endFrames > startFrames,
                       QStringLiteral("观看期间没有持续解码（%1 → %2）").arg(startFrames).arg(endFrames));
        // A near-frozen viewer must not be reported as a normal pass just
        // because a handful of frames arrived. The threshold is deliberately
        // low because bgra-tiles sends changed regions rather than full frames,
        // but ~1 fps with 50 s latency is a failed display, not a pass.
        const double observationSeconds = sampleTimer.elapsed() / 1000.0;
        const double observedFps = observationSeconds > 0
            ? double(endFrames - startFrames) / observationSeconds : 0.0;
        result.stats.insert(QStringLiteral("observedFps"), observedFps);
        result.require(observedFps >= 5.0,
                       QStringLiteral("观看期间显示近乎冻结（%1 fps，%2 → %3）")
                           .arg(observedFps, 0, 'f', 2).arg(startFrames).arg(endFrames));
    }
    const auto stats = controllerStats(controller);
    result.stats.insert(QStringLiteral("stats"), stats);
    result.stats.insert(QStringLiteral("decodedFrames"), endFrames);
    result.stats.insert(QStringLiteral("frameWidth"), controller->property("frameWidth").toInt());
    result.stats.insert(QStringLiteral("frameHeight"), controller->property("frameHeight").toInt());
    result.require(stats.value(QStringLiteral("datagramsSent")).toString().toULongLong() == 0,
                   QStringLiteral("观看者不应发送视频报文"));
    result.require(stats.value(QStringLiteral("completePackets")).toString().toULongLong() > 0,
                   QStringLiteral("观看者没有收到完整编码包"));
    result.require(stats.value(QStringLiteral("networkSubmitted")).toString().toULongLong() > 0,
                   QStringLiteral("观看者没有提交编码包到本地轨道"));
    // Footer evidence for the whole viewing window: actual controller error, the
    // visible page labels and the feedback cadence counters.
    QVariantMap feedbackEnd;
    QMetaObject::invokeMethod(controller, "feedbackObservation", Q_RETURN_ARG(QVariantMap, feedbackEnd));
    result.stats.insert(QStringLiteral("feedback"), feedbackEnd);
    result.stats.insert(QStringLiteral("errorTextAtEnd"), controller->property("errorText").toString());
    result.stats.insert(QStringLiteral("detailsAtEnd"), controller->property("details").toString());
    if (auto *error = fixture.page->findChild<QQuickItem *>(QStringLiteral("desktopShareError"))) {
        result.stats.insert(QStringLiteral("pageErrorTextAtEnd"), error->property("text").toString());
        result.stats.insert(QStringLiteral("pageErrorVisibleAtEnd"), error->isVisible());
    }
    if (auto *details = fixture.page->findChild<QQuickItem *>(QStringLiteral("desktopShareDetails"))) {
        result.stats.insert(QStringLiteral("pageDetailsTextAtEnd"), details->property("text").toString());
        result.stats.insert(QStringLiteral("pageDetailsVisibleAtEnd"), details->isVisible());
    }
    result.stats.insert(QStringLiteral("feedbackSupported"), feedbackSupported);
    result.stats.insert(QStringLiteral("rateLimitedVisible"), rateLimitedSeen);
    result.require(!rateLimitedSeen,
                   QStringLiteral("正常观看页面出现 RATE_LIMITED：%1 / %2")
                       .arg(result.stats.value(QStringLiteral("pageErrorTextAtEnd")).toString(),
                            result.stats.value(QStringLiteral("pageDetailsTextAtEnd")).toString()));
    if (feedbackSupported) {
        result.require(feedbackEnd.value(QStringLiteral("responses")).toString().toULongLong() > 0,
                       QStringLiteral("观看期间没有收到任何反馈确认"));
        result.require(feedbackEnd.value(QStringLiteral("rateLimited")).toString().toULongLong() == 0,
                       QStringLiteral("正常观看收到 RATE_LIMITED（反馈节流误判）"));
        result.require(feedbackEnd.value(QStringLiteral("minAckToRequestMs")).toString().toDouble() > 0.0,
                       QStringLiteral("反馈节流未记录确认到下一次请求的间隔"));
    }

    QMetaObject::invokeMethod(controller, "leaveRoom");
    const qint64 leaveRequestedEpochMs = QDateTime::currentMSecsSinceEpoch();
    result.require(waitFor([&] { return !controller->property("watching").toBool(); }, 15000),
                   QStringLiteral("退出观看未完成：%1").arg(controller->property("statusText").toString()));
    result.stats.insert(QStringLiteral("leaveRequestedEpochMs"), double(leaveRequestedEpochMs));
    result.stats.insert(QStringLiteral("leaveCompletedEpochMs"),
                        double(QDateTime::currentMSecsSinceEpoch()));
    const int stoppedFrames = controller->property("decodedFrames").toInt();
    pump(1500);
    result.require(controller->property("decodedFrames").toInt() == stoppedFrames,
                   QStringLiteral("退出观看后仍收到旧画面"));
    result.stats.insert(QStringLiteral("stoppedFrames"), stoppedFrames);
    // Post-leave snapshot so each viewer result also records the queue/reassembly
    // counters after the session ended.
    const auto leftStats = controllerStats(controller);
    result.stats.insert(QStringLiteral("statsAfterLeave"), leftStats);
    result.require(leftStats.value(QStringLiteral("inboundQueued")).toInt() == 0,
                   QStringLiteral("退出观看后仍有排队媒体包"));
    result.require(leftStats.value(QStringLiteral("pendingPackets")).toInt() == 0,
                   QStringLiteral("退出观看后仍有未完成重组"));
    return finish(result, env("ZDS_E2E_OUT"), fixture);
}

// Decoder calibration: exact value recovery after scaling, known-latency
// arithmetic including delays above one second, a frozen-publisher case, and
// rejection of images without a valid barcode.
int runCalibration()
{
    QVariantList cases;
    QVariantList failures;
    bool ok = true;
    const QList<QSize> sizes = {QSize(480, 270), QSize(868, 253), QSize(450, 253),
                                QSize(1800, 506), QSize(300, 169), QSize(1280, 720)};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QList<qint64> values = {0, 1, 999, 1000, 1120, 2120, 5000, 123456, 0xFFFFFF,
                                  now & kTimeCodeMask, (now - 2500) & kTimeCodeMask};
    const auto record = [&](const QString &name, bool passed, const QString &detail) {
        QVariantMap entry;
        entry.insert(QStringLiteral("case"), name);
        entry.insert(QStringLiteral("passed"), passed);
        entry.insert(QStringLiteral("detail"), detail);
        cases.append(entry);
        if (!passed) {
            failures.append(entry);
            ok = false;
        }
    };
    for (qint64 value : values) {
        const auto base = renderTimeCodeImage(value);
        for (const auto &size : sizes) {
            const auto scaled = base.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            qint64 decoded = 0;
            const bool decodedOk = decodeTimeCode(scaled, &decoded);
            record(QStringLiteral("decode %1 at %2x%3").arg(value).arg(size.width()).arg(size.height()),
                   decodedOk && decoded == value,
                   QStringLiteral("decoded=%1 expected=%2 ok=%3").arg(decoded).arg(value).arg(decodedOk));
        }
    }
    for (qint64 delay : QList<qint64>{0, 120, 1120, 2120, 5500}) {
        const qint64 publisher = (now - delay) & kTimeCodeMask;
        qint64 measured = -1;
        const bool accepted = timeCodeLatencyMs(now, publisher, &measured);
        record(QStringLiteral("latency %1ms").arg(delay), accepted && measured == delay,
               QStringLiteral("measured=%1 accepted=%2").arg(measured).arg(accepted));
    }
    {
        const auto base = renderTimeCodeImage((now - 2500) & kTimeCodeMask);
        const auto scaled = base.scaled(QSize(868, 253), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        qint64 decoded = 0;
        qint64 measured = -1;
        const bool decodedOk = decodeTimeCode(scaled, &decoded);
        const bool accepted = decodedOk && timeCodeLatencyMs(now, decoded, &measured);
        record(QStringLiteral("frozen publisher over one second"), accepted && measured == 2500,
               QStringLiteral("decoded=%1 measured=%2").arg(decoded).arg(measured));
    }
    {
        QImage plain(480, 270, QImage::Format_RGB32);
        plain.fill(QColor(230, 30, 200));
        qint64 decoded = 0;
        record(QStringLiteral("plain image rejected"), !decodeTimeCode(plain, &decoded),
               QStringLiteral("decoded=%1").arg(decoded));
        qint64 measured = -1;
        record(QStringLiteral("clock skew rejected"),
               !timeCodeLatencyMs(now, (now + 1500) & kTimeCodeMask, &measured),
               QStringLiteral("measured=%1").arg(measured));
    }
    QVariantMap result;
    result.insert(QStringLiteral("ok"), ok);
    result.insert(QStringLiteral("cases"), cases);
    result.insert(QStringLiteral("failures"), failures);
    result.insert(QStringLiteral("caseCount"), cases.size());
    result.insert(QStringLiteral("failureCount"), failures.size());
    writeJson(env("ZDS_E2E_OUT"), result);
    for (const auto &entry : cases) {
        const auto map = entry.toMap();
        std::fprintf(stderr, "[calibrate] %s: %s (%s)\n",
                     map.value(QStringLiteral("passed")).toBool() ? "PASS" : "FAIL",
                     map.value(QStringLiteral("case")).toString().toUtf8().constData(),
                     map.value(QStringLiteral("detail")).toString().toUtf8().constData());
    }
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QUICK_BACKEND", "software");
    QApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
#ifdef Q_OS_WIN
    qputenv("QT_QPA_FONTDIR", (qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts").toLocal8Bit());
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/msyh.ttc");
    app.setFont(QFont(QStringLiteral("Microsoft YaHei"), 10));
#endif
    const auto role = env("ZDS_E2E_ROLE", QStringLiteral("viewer"));
    const int code = role == QStringLiteral("calibrate") ? runCalibration()
        : role == QStringLiteral("publisher") ? runPublisher() : runViewer();
    std::fprintf(stderr, "[e2e] main: role finished %d\n", code);
    std::fflush(stderr);
    return code;
}
