#include "VideoCodec.h"
#include "VideoCanvas.h"
#include "VideoFramePacer.h"
#include "AudioMath.h"
#include "MediaClock.h"
#include "ClipAudio.h"
#include "ClipPlayer.h"
#include <ztmedia/SharedFrames.h>
#include <ztprotocol/BulkChannel.h>
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QtEndian>
#include <QPainter>
using namespace vusiif::zto;
namespace {
// Deterministic test picture: four colour blocks, a white grid, a cyan circle
// and a white label on a black band. It is used both directly on the composed
// canvas and through the H.264 encoder/decoder.
QImage testPattern(int width, int height)
{
    QImage image(width, height, QImage::Format_ARGB32);
    if (image.isNull())
        return image;
    image.fill(Qt::black);
    QPainter painter(&image);
    const int halfWidth = width / 2, halfHeight = height / 2;
    painter.fillRect(QRect(0, 0, halfWidth, halfHeight), QColor(220, 30, 30));
    painter.fillRect(QRect(halfWidth, 0, width - halfWidth, halfHeight), QColor(30, 200, 60));
    painter.fillRect(QRect(0, halfHeight, halfWidth, height - halfHeight), QColor(40, 60, 220));
    painter.fillRect(QRect(halfWidth, halfHeight, width - halfWidth, height - halfHeight),
                     QColor(230, 210, 40));
    painter.setPen(QPen(Qt::white, std::max(1, width / 160)));
    for (int i = 1; i < 8; ++i) {
        painter.drawLine(width * i / 8, 0, width * i / 8, height);
        painter.drawLine(0, height * i / 8, width, height * i / 8);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 220, 220));
    const auto radius = std::min(width, height) / 6.0;
    painter.drawEllipse(QPointF(width / 2.0, height / 2.0), radius, radius);
    const int bandTop = height * 86 / 100;
    painter.fillRect(QRect(0, bandTop, width, height - bandTop), Qt::black);
    painter.setPen(Qt::white);
    QFont font(QStringLiteral("Arial"));
    font.setPixelSize(std::max(8, (height - bandTop) * 3 / 4));
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(QRect(0, bandTop, width, height - bandTop), Qt::AlignCenter,
                     QStringLiteral("ZTOrbit"));
    painter.end();
    return image;
}
// Content rectangle of an aspect-preserving composition; used to locate sample
// points inside the picture, not to re-implement the assertion.
QRect contentRect(QSize source, QSize canvas)
{
    const auto scaled = source.scaled(canvas, Qt::KeepAspectRatio);
    return QRect(QPoint((canvas.width() - scaled.width()) / 2,
                        (canvas.height() - scaled.height()) / 2),
                 scaled);
}
QColor sampleColor(const QImage &image, const QRect &content, int xNum, int xDen, int yNum, int yDen)
{
    const int x = std::clamp(content.x() + content.width() * xNum / xDen, 0, image.width() - 1);
    const int y = std::clamp(content.y() + content.height() * yNum / yDen, 0, image.height() - 1);
    return image.pixelColor(x, y);
}
bool colorNear(const QColor &actual, const QColor &expected, int tolerance)
{
    return std::abs(actual.red() - expected.red()) <= tolerance &&
           std::abs(actual.green() - expected.green()) <= tolerance &&
           std::abs(actual.blue() - expected.blue()) <= tolerance;
}
int brightPixels(const QImage &image, const QRect &area, int threshold = 200)
{
    int count = 0;
    const auto rect = area.intersected(image.rect());
    for (int y = rect.top(); y <= rect.bottom(); y += 2)
        for (int x = rect.left(); x <= rect.right(); x += 2)
            if (image.pixelColor(x, y).lightness() > threshold)
                ++count;
    return count;
}
QByteArray makeWav(int frames, quint16 format = 1, quint16 channels = 1, quint32 rate = 48000,
                   quint16 bits = 16, int dataBytes = -1, bool padding = false)
{
    const int data = dataBytes >= 0 ? dataBytes : frames * (bits / 8) * channels;
    QByteArray wav("RIFF", 4);
    QByteArray body;
    body += "fmt ";
    QByteArray fmtSize(4, '\0');
    qToLittleEndian(quint32(16), reinterpret_cast<uchar *>(fmtSize.data()));
    body += fmtSize;
    QByteArray fmt(16, '\0');
    qToLittleEndian(format, reinterpret_cast<uchar *>(fmt.data()));
    qToLittleEndian(channels, reinterpret_cast<uchar *>(fmt.data() + 2));
    qToLittleEndian(rate, reinterpret_cast<uchar *>(fmt.data() + 4));
    qToLittleEndian(rate * channels * (bits / 8), reinterpret_cast<uchar *>(fmt.data() + 8));
    qToLittleEndian(quint16(channels * (bits / 8)), reinterpret_cast<uchar *>(fmt.data() + 12));
    qToLittleEndian(bits, reinterpret_cast<uchar *>(fmt.data() + 14));
    body += fmt;
    body += "data";
    QByteArray length(4, '\0');
    qToLittleEndian(quint32(data), reinterpret_cast<uchar *>(length.data()));
    body += length;
    QByteArray samples(data, '\0');
    for (int i = 0; i + 1 < data; i += 2) {
        const qint16 value = qint16((i / 2) % 2000 - 1000);
        qToLittleEndian(quint16(value), reinterpret_cast<uchar *>(samples.data() + i));
    }
    body += samples;
    if (padding) body += '\0';
    QByteArray size(4, '\0');
    qToLittleEndian(quint32(body.size() + 4), reinterpret_cast<uchar *>(size.data()));
    wav += size;
    wav += "WAVE";
    wav += body;
    return wav;
}
}
class MediaEngineTests : public QObject {
    Q_OBJECT
  private slots:
    void capturePacingRateBound_data() {
        QTest::addColumn<int>("hz");
        for (int hz : {1, 10, 24, 30, 60, 120, 1000})
            QTest::newRow(qPrintable(QString::number(hz))) << hz;
    }
    void capturePacingRateBound() {
        QFETCH(int, hz);
        media::VideoFramePacer pacer;
        int admitted = 0;
        for (int i = 0; i < hz * 60; ++i) {
            const qint64 pts = qint64(i) * 1000000 / hz;
            admitted += pacer.admit(pts, 30);
            if (i == hz * 10 - 1)
                qInfo() << "inputHz" << hz << "admittedIn10s" << admitted << "configuredHz" << 30;
            // Prefix conservation, including the two-frame burst allowance.
            QVERIFY2(admitted <= 2 + pts * 30 / 1000000, "duplicated elapsed-time credit");
        }
        QVERIFY(admitted >= std::min(hz, 30) * 60 - 1);
        qInfo() << "inputHz" << hz << "admittedIn60s" << admitted;
    }
    void capturePacingJitterAndBursts() {
        media::VideoFramePacer pacer;
        int admitted = 0;
        qint64 previous = -1;
        // Alternating early/late 30Hz arrivals and 12-frame microbursts.
        for (int i = 0; i < 1800; ++i) {
            const qint64 pts = qint64(i) * 1000000 / 30 + (i % 2 ? 6000 : 0);
            admitted += pacer.admit(pts, 30);
            for (int b = 1; b <= 12; ++b) admitted += pacer.admit(pts + b, 30);
            QVERIFY(admitted <= 2 + (pts + 12) * 30 / 1000000);
            previous = pts + 12;
        }
        QVERIFY(admitted >= 1798);
        // A long idle period can bank only two frames, never minutes of work.
        const auto later = previous + 3600000000LL;
        int burst = 0;
        for (int i = 0; i < 1000; ++i) burst += pacer.admit(later + i, 30);
        QCOMPARE(burst, 2);
    }
    void capturePacingTimestampAndLifetime() {
        media::VideoFramePacer pacer;
        QVERIFY(!pacer.admit(-1, 30));
        QVERIFY(pacer.admit(0, 30));
        QVERIFY(!pacer.admit(0, 30));
        QVERIFY(pacer.admit(1, 30));
        QVERIFY(!pacer.admit(10000, 30));
        QVERIFY(!pacer.admit(9000, 30));
        QVERIFY(!pacer.admit(20000, 30));
        QCOMPARE(pacer.lastEncodedPts(), qint64(1));
        QVERIFY(pacer.admit(33334, 30));
        for (int i = 0; i < 1000; ++i) QVERIFY(!pacer.admit(33334, 30));
        QVERIFY(!pacer.admit(1, 30));
        QVERIFY(pacer.admit(66667, 30));
        pacer.reset(); // stop/start resets the two-frame budget and PTS epoch.
        QVERIFY(pacer.admit(0, 30));
        QVERIFY(pacer.admit(1, 30));
        // Extreme pauses saturate without arithmetic overflow.
        QVERIFY(pacer.admit(std::numeric_limits<qint64>::max() - 1, 30));
        QVERIFY(pacer.admit(std::numeric_limits<qint64>::max(), 30));
        QVERIFY(!pacer.admit(std::numeric_limits<qint64>::max(), 30));
    }
    void canvasPreservesAspectRatio_data() {
        QTest::addColumn<int>("sourceWidth");
        QTest::addColumn<int>("sourceHeight");
        QTest::addColumn<int>("canvasWidth");
        QTest::addColumn<int>("canvasHeight");
        QTest::newRow("4:3-720p") << 640 << 480 << 1280 << 720;
        QTest::newRow("16:10-720p") << 1280 << 800 << 1280 << 720;
        QTest::newRow("ultrawide-720p") << 2560 << 1080 << 1280 << 720;
        QTest::newRow("portrait-720p") << 480 << 854 << 1280 << 720;
        QTest::newRow("non-integer-720p") << 1000 << 563 << 1280 << 720;
        QTest::newRow("4:3-1080p") << 640 << 480 << 1920 << 1080;
    }
    void canvasPreservesAspectRatio() {
        QFETCH(int, sourceWidth);
        QFETCH(int, sourceHeight);
        QFETCH(int, canvasWidth);
        QFETCH(int, canvasHeight);
        const auto source = testPattern(sourceWidth, sourceHeight);
        const auto canvas = media::engine::composeVideoCanvas(source, canvasWidth, canvasHeight);
        QVERIFY(!canvas.isNull());
        QCOMPARE(canvas.size(), QSize(canvasWidth, canvasHeight));
        QCOMPARE(canvas.format(), QImage::Format_ARGB32);
        const auto content = contentRect(QSize(sourceWidth, sourceHeight),
                                         QSize(canvasWidth, canvasHeight));
        QVERIFY(content.width() > 8 && content.height() > 8);
        // The composed content keeps the source aspect ratio instead of being
        // stretched to the track format.
        QVERIFY(qAbs(double(content.width()) / content.height() -
                     double(sourceWidth) / sourceHeight) < 0.02);
        // The unused area is a black bar on the corresponding side.
        if (content.left() > 5) {
            QCOMPARE(canvas.pixelColor(1, canvasHeight / 2), QColor(Qt::black));
            QCOMPARE(canvas.pixelColor(content.left() - 3, canvasHeight / 2), QColor(Qt::black));
        }
        if (content.top() > 5) {
            QCOMPARE(canvas.pixelColor(canvasWidth / 2, 1), QColor(Qt::black));
            QCOMPARE(canvas.pixelColor(canvasWidth / 2, content.top() - 3), QColor(Qt::black));
        }
        if (content.right() < canvasWidth - 5)
            QCOMPARE(canvas.pixelColor(canvasWidth - 2, canvasHeight / 2), QColor(Qt::black));
        if (content.bottom() < canvasHeight - 5)
            QCOMPARE(canvas.pixelColor(canvasWidth / 2, canvasHeight - 2), QColor(Qt::black));
        // Colour blocks keep their quadrant positions.
        QVERIFY(colorNear(sampleColor(canvas, content, 1, 16, 1, 16), QColor(220, 30, 30), 2));
        QVERIFY(colorNear(sampleColor(canvas, content, 15, 16, 1, 16), QColor(30, 200, 60), 2));
        QVERIFY(colorNear(sampleColor(canvas, content, 1, 16, 13, 16), QColor(40, 60, 220), 2));
        QVERIFY(colorNear(sampleColor(canvas, content, 15, 16, 13, 16), QColor(230, 210, 40), 2));
        // Circle, grid line and label survive the composition.
        QVERIFY(colorNear(sampleColor(canvas, content, 1, 2, 1, 2), QColor(0, 220, 220), 2));
        const auto grid = sampleColor(canvas, content, 3, 8, 1, 16);
        QVERIFY2(grid.lightness() > 220, qPrintable(grid.name()));
        const QRect band(content.x(), content.y() + content.height() * 88 / 100, content.width(),
                         content.height() * 12 / 100);
        QVERIFY2(brightPixels(canvas, band) > 20, "label text lost");
    }
    void canvasHandlesMissingOrDegenerateSource() {
        using namespace media::engine;
        const auto blank = composeVideoCanvas(QImage(), 1280, 720);
        QCOMPARE(blank.size(), QSize(1280, 720));
        QCOMPARE(blank.pixelColor(0, 0), QColor(Qt::black));
        QCOMPARE(blank.pixelColor(1279, 719), QColor(Qt::black));
        QVERIFY(composeVideoCanvas(testPattern(64, 64), 1, 720).isNull());
        QVERIFY(composeVideoCanvas(testPattern(64, 64), 1280, 1).isNull());
        const auto same = composeVideoCanvas(testPattern(1280, 720), 1280, 720);
        QCOMPARE(same.size(), QSize(1280, 720));
        QCOMPARE(same.format(), QImage::Format_ARGB32);
    }
    void videoProfilesKeepCanvasThroughCodec_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<int>("bitrate");
        QTest::addColumn<QString>("name");
        QTest::newRow("720p30") << 1280 << 720 << 2000000 << QStringLiteral("720p30");
        QTest::newRow("1080p30") << 1920 << 1080 << 4000000 << QStringLiteral("1080p30");
    }
    void videoProfilesKeepCanvasThroughCodec() {
        using namespace media::engine;
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(int, bitrate);
        QFETCH(QString, name);
        ExtensionRegistry registry;
        media::TrackConfig config;
        config.kind = "video";
        config.direction = "send";
        config.source = "synthetic";
        config.video.width = width;
        config.video.height = height;
        config.codec.codec = "h264";
        config.codec.bitrate = bitrate;
        config.codec.frameRate = 30;
        config.codec.keyFrameInterval = 30;
        VideoCodec encoder, decoder;
        QVERIFY2(encoder.open(config, registry).isEmpty(), qPrintable(name));
        auto receive = config;
        receive.direction = "receive";
        receive.source.clear();
        QVERIFY2(decoder.open(receive, registry).isEmpty(), qPrintable(name));
        const auto canvas = media::engine::composeVideoCanvas(testPattern(640, 480), width, height);
        QVERIFY(!canvas.isNull());
        const auto markerArea = contentRect(QSize(640,480),QSize(width,height));
        QString error;
        QList<media::EncodedPacket> packets;
        int decoded = 0;
        QImage last;
        for (int frame = 0; frame < 10; ++frame) {
            // A moving marker keeps the encoder from skipping identical frames,
            // so keyframe requests can be observed.
            auto picture = canvas;
            QPainter marker(&picture);
            // Keep animation inside the content. The old x=2 marker painted
            // over the black-bar assertion in frame 9; decoder frame threading
            // used to conceal this by withholding the final decoded picture.
            marker.fillRect(QRect(markerArea.x()+markerArea.width()*3/4,
                                  markerArea.y()+(frame*37)%(markerArea.height()-48),40,40),Qt::white);
            marker.end();
            if (frame == 5)
                encoder.keyFrame();
            for (auto &packet : encoder.encode(picture, qint64(frame) * 33333, error)) {
                QVERIFY2(error.isEmpty(), qPrintable(error));
                QCOMPARE(packet.codec, QStringLiteral("h264"));
                QCOMPARE(packet.duration, qint64(33333));
                QCOMPARE(packet.timeBase.denominator, 1000000);
                packets.append(packet);
                for (auto &frameImage : decoder.decode(packet, error)) {
                    QVERIFY2(error.isEmpty(), qPrintable(error));
                    ++decoded;
                    last = frameImage.first;
                }
            }
        }
        QVERIFY(error.isEmpty());
        QVERIFY(decoded > 0);
        QVERIFY(!packets.isEmpty());
        QVERIFY2(packets.first().keyFrame, "first packet must be a key frame");
        bool requestedDueKeyFrame = false;
        for (qsizetype i = 1; i < packets.size(); ++i)
            if (packets[i].keyFrame && packets[i].sequence > packets.first().sequence)
                requestedDueKeyFrame = true;
        QVERIFY2(requestedDueKeyFrame, "requestKeyFrame must produce a new IDR packet");
        QVERIFY(!last.isNull());
        QCOMPARE(last.size(), QSize(width, height));
        const auto content = contentRect(QSize(640, 480), QSize(width, height));
        QCOMPARE(content.size().height(), height);
        if (content.left() > 5)
            QVERIFY(last.pixelColor(2, height / 2).lightness() < 30);
        QVERIFY(colorNear(sampleColor(last, content, 1, 16, 1, 16), QColor(220, 30, 30), 45));
        QVERIFY(colorNear(sampleColor(last, content, 15, 16, 1, 16), QColor(30, 200, 60), 45));
        QVERIFY(colorNear(sampleColor(last, content, 1, 16, 13, 16), QColor(40, 60, 220), 45));
        QVERIFY(colorNear(sampleColor(last, content, 15, 16, 13, 16), QColor(230, 210, 40), 45));
        QVERIFY(colorNear(sampleColor(last, content, 1, 2, 1, 2), QColor(0, 220, 220), 45));
        const QRect band(content.x(), content.y() + content.height() * 88 / 100, content.width(),
                         content.height() * 12 / 100);
        QVERIFY2(brightPixels(last, band, 180) > 10, "label text lost through H.264");
    }
    void firstKeyFrameDecodesWithoutFollowingPacket_data() {
        videoProfilesKeepCanvasThroughCodec_data();
    }
    void firstKeyFrameDecodesWithoutFollowingPacket() {
        using namespace media::engine;
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(int, bitrate);
        ExtensionRegistry registry;
        media::TrackConfig config;
        config.kind = "video";
        config.codec.codec = "h264";
        config.codec.bitrate = bitrate;
        config.video.width = width;
        config.video.height = height;
        VideoCodec encoder, decoder;
        QVERIFY(encoder.open(config, registry).isEmpty());
        config.direction = "receive";
        QVERIFY(decoder.open(config, registry).isEmpty());
        QString error;
        const auto packets = encoder.encode(testPattern(width, height), 1000000, error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!packets.isEmpty());
        QVERIFY(packets.first().keyFrame);
        const auto frames = decoder.decode(packets.first(), error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(!frames.isEmpty(), "A static source must not need another packet to display its first IDR");
        QCOMPARE(frames.first().first.size(), QSize(width, height));
        QCOMPARE(frames.first().second, qint64(1000000));
    }
    void forcedKeyFrameKeepsActualCaptureTimestamp() {
        using namespace media::engine;
        ExtensionRegistry registry;media::TrackConfig config;
        config.kind="video";config.codec.codec="h264";config.codec.frameRate=1;
        config.codec.bitrate=500000;config.video.width=320;config.video.height=180;
        VideoCodec encoder,decoder;QVERIFY(encoder.open(config,registry).isEmpty());
        config.direction="receive";QVERIFY(decoder.open(config,registry).isEmpty());
        QImage image(320,180,QImage::Format_ARGB32);image.fill(Qt::blue);QString error;
        const auto initial=encoder.encode(image,1000000,error);QVERIFY(!initial.isEmpty());
        QVERIFY(!decoder.decode(initial.first(),error).isEmpty());
        encoder.keyFrame();const auto refresh=encoder.encode(image,1100000,error);
        QVERIFY2(error.isEmpty(),qPrintable(error));QVERIFY(!refresh.isEmpty());QVERIFY(refresh.first().keyFrame);
        QCOMPARE(refresh.first().pts,qint64(1100000));
        const auto frames=decoder.decode(refresh.first(),error);QVERIFY(!frames.isEmpty());
        QCOMPARE(frames.first().second,qint64(1100000));
    }
    void packetMetadataRoundtrip() {
        media::EncodedPacket p;
        p.track = "epoch-track";
        p.codec = "h264";
        p.generation = 9007199254740993ULL;
        p.sequence = 18446744073709551614ULL;
        p.pts = 9223372036854775000LL;
        p.dts = -42;
        p.initializationData = "codec";
        p.data = "packet";
        p.keyFrame = true;
        auto copy = media::encodedPacket(media::toJson(p), p.data);
        QCOMPARE(copy.generation, p.generation);
        QCOMPARE(copy.sequence, p.sequence);
        QCOMPARE(copy.pts, p.pts);
        QCOMPARE(copy.dts, p.dts);
        QCOMPARE(copy.data, p.data);
    }
    void clipWavValidation() {
        using namespace media::engine;
        const auto valid = parseClipWav(makeWav(7680));
        QVERIFY2(valid.ok(), qPrintable(valid.error));
        QCOMPARE(valid.samples.size(), 7680);
        QVERIFY(qAbs(valid.samples.first() - (-1000.0f / 32768.0f)) < 1e-6f);
        const auto padded = parseClipWav(makeWav(960, 1, 1, 48000, 16, 960, true));
        QVERIFY2(padded.ok(), qPrintable(padded.error));
        QCOMPARE(padded.samples.size(), 480);

        const auto expectError = [](const QByteArray &wav) {
            const auto audio = parseClipWav(wav);
            return !audio.ok() && !audio.error.isEmpty();
        };
        QVERIFY(expectError({}));
        QVERIFY(expectError(QByteArray(100, 'x')));
        auto truncated = makeWav(480);
        truncated.resize(truncated.size() - 10);
        QVERIFY(expectError(truncated));
        QVERIFY(expectError(makeWav(480, 3))); // float format
        QVERIFY(expectError(makeWav(480, 1, 2))); // stereo
        QVERIFY(expectError(makeWav(480, 1, 1, 44100))); // wrong rate
        QVERIFY(expectError(makeWav(480, 1, 1, 48000, 8))); // wrong depth
        QVERIFY(expectError(makeWav(480, 1, 1, 48000, 16, 3))); // odd PCM16 data
        QVERIFY(expectError(makeWav(48001 * 2))); // more than two seconds
        QVERIFY(expectError(QByteArray(256 * 1024 + 1, 'x')));
        auto huge = makeWav(480);
        const quint32 hugeSize = 0x7fffffffu;
        qToLittleEndian(hugeSize, reinterpret_cast<uchar *>(huge.data() + 40));
        QVERIFY(expectError(huge));
        auto duplicate = makeWav(480);
        duplicate += "data";
        duplicate += QByteArray(4, '\0');
        QVERIFY(expectError(duplicate));
    }
    void clipPlayerEmitsOneFailureAndStopsFeeding()
    {
        using namespace media::engine;
        QVector<QVector<float>> chunks;
        for (int i = 0; i < 6; ++i) chunks.append(QVector<float>(960, 0.1f));
        int fed = 0, finished = 0, failed = 0;
        QString message;
        ClipPlayer::Hooks hooks;
        hooks.feed = [&](QVector<float>, std::function<void(bool)> done) {
            ++fed;
            done(fed < 2); // The second chunk is rejected by the output.
        };
        hooks.query = [](std::function<void(const ClipPlayer::DrainState &)> done) {
            done(ClipPlayer::DrainState{});
        };
        hooks.finished = [&] { ++finished; };
        hooks.failed = [&](const QString &reason) { ++failed; message = reason; };
        ClipPlayer player("clip-1", "lease", chunks, hooks);
        player.start();
        QTRY_COMPARE_WITH_TIMEOUT(failed, 1, 2000);
        QCOMPARE(fed, 2);
        QCOMPARE(finished, 0);
        QVERIFY(!message.isEmpty());
        QTest::qWait(80); // No retries or a second terminal event.
        QCOMPARE(fed, 2);
        QCOMPARE(failed, 1);
        QCOMPARE(finished, 0);
        QVERIFY(player.terminal());
    }
    void clipPlayerWaitsForTheMixQueueAndSinkBacklog()
    {
        using namespace media::engine;
        QVector<QVector<float>> chunks;
        for (int i = 0; i < 4; ++i) chunks.append(QVector<float>(960, 0.1f));
        int fed = 0, finished = 0, failed = 0, queries = 0;
        ClipPlayer::Hooks hooks;
        hooks.feed = [&](QVector<float>, std::function<void(bool)> done) {
            ++fed;
            QTimer::singleShot(0, this, [done] { done(true); });
        };
        hooks.query = [&](std::function<void(const ClipPlayer::DrainState &)> done) {
            ++queries;
            QTimer::singleShot(0, this, [&, done] {
                ClipPlayer::DrainState state;
                state.queuedFrames = queries < 5 ? 1 : 0;
                state.pendingBytes = queries < 6 ? 32 : 0;
                done(state);
            });
        };
        hooks.finished = [&] { ++finished; };
        hooks.failed = [&](const QString &) { ++failed; };
        ClipPlayer player("clip-2", "lease", chunks, hooks);
        player.setDrainTimeoutMs(3000);
        player.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished, 1, 3000);
        QCOMPARE(fed, 4);
        QCOMPARE(failed, 0);
        QVERIFY(queries >= 6);
        // The terminal event is emitted exactly once.
        QTest::qWait(60);
        QCOMPARE(finished, 1);
    }
    void clipPlayerFailsWhenTheOutputDisappears()
    {
        using namespace media::engine;
        QVector<QVector<float>> chunks;
        chunks.append(QVector<float>(960, 0.1f));
        int finished = 0, failed = 0;
        QString message;
        ClipPlayer::Hooks hooks;
        hooks.feed = [](QVector<float>, std::function<void(bool)> done) { done(true); };
        hooks.query = [](std::function<void(const ClipPlayer::DrainState &)> done) {
            ClipPlayer::DrainState state;
            state.output = false;
            state.sinkActive = false;
            done(state);
        };
        hooks.finished = [&] { ++finished; };
        hooks.failed = [&](const QString &reason) { ++failed; message = reason; };
        ClipPlayer player("clip-3", "lease", chunks, hooks);
        player.setDrainTimeoutMs(500);
        player.start();
        QTRY_COMPARE_WITH_TIMEOUT(failed, 1, 2000);
        QCOMPARE(finished, 0);
        QVERIFY(message.contains(QStringLiteral("输出")));
    }
    void clipPlayerTimesOutInsteadOfBlockingTheQueue()
    {
        using namespace media::engine;
        QVector<QVector<float>> chunks;
        chunks.append(QVector<float>(960, 0.1f));
        int finished = 0, failed = 0;
        QString message;
        ClipPlayer::Hooks hooks;
        hooks.feed = [](QVector<float>, std::function<void(bool)> done) { done(true); };
        hooks.query = [](std::function<void(const ClipPlayer::DrainState &)> done) {
            ClipPlayer::DrainState state;
            state.queuedFrames = 1; // The sink never consumes the data.
            done(state);
        };
        hooks.finished = [&] { ++finished; };
        hooks.failed = [&](const QString &reason) { ++failed; message = reason; };
        ClipPlayer player("clip-4", "lease", chunks, hooks);
        player.setDrainTimeoutMs(80);
        player.start();
        QTRY_COMPARE_WITH_TIMEOUT(failed, 1, 2000);
        QCOMPARE(finished, 0);
        QVERIFY(message.contains(QStringLiteral("排空")));
    }
    void clipPlayerWatchdogFailsWhenTheDeviceReplyIsLost()
    {
        using namespace media::engine;
        QVector<QVector<float>> chunks;
        chunks.append(QVector<float>(960, 0.1f));
        int finished = 0, failed = 0;
        QString message;
        ClipPlayer::Hooks hooks;
        hooks.feed = [](QVector<float>, std::function<void(bool)> done) {
            // Never complete: a lost device-thread reply must not block forever.
            Q_UNUSED(done);
        };
        hooks.query = [](std::function<void(const ClipPlayer::DrainState &)> done) {
            done(ClipPlayer::DrainState{});
        };
        hooks.finished = [&] { ++finished; };
        hooks.failed = [&](const QString &reason) { ++failed; message = reason; };
        ClipPlayer player("clip-6", "lease", chunks, hooks);
        player.setWatchdogTimeoutMs(40);
        player.start();
        QTRY_COMPARE_WITH_TIMEOUT(failed, 1, 2000);
        QCOMPARE(finished, 0);
        QVERIFY(message.contains(QStringLiteral("超时")));
    }
    void clipPlayerStopIsSilent()
    {
        using namespace media::engine;
        QVector<QVector<float>> chunks;
        for (int i = 0; i < 4; ++i) chunks.append(QVector<float>(960, 0.1f));
        int finished = 0, failed = 0;
        ClipPlayer::Hooks hooks;
        hooks.feed = [&](QVector<float>, std::function<void(bool)> done) {
            QTimer::singleShot(0, this, [done] { done(true); });
        };
        hooks.query = [](std::function<void(const ClipPlayer::DrainState &)> done) {
            ClipPlayer::DrainState state;
            state.queuedFrames = 1;
            done(state);
        };
        hooks.finished = [&] { ++finished; };
        hooks.failed = [&](const QString &) { ++failed; };
        ClipPlayer player("clip-5", "lease", chunks, hooks);
        player.start();
        QTest::qWait(30);
        player.stop();
        QTest::qWait(80);
        QCOMPARE(finished, 0);
        QCOMPARE(failed, 0);
        QVERIFY(player.stopped());
    }
    void audioVideoShareClockDomain() {
        media::engine::PlaybackTimeline clock;
        auto audio=clock.schedule("peer-1",48000,{1,48000},5000000);
        auto video=clock.schedule("peer-1",90000,{1,90000},5010000);
        QCOMPARE(audio,qint64(5060000));QCOMPARE(video,audio);
        QCOMPARE(clock.schedule("peer-1",99000,{1,90000},5020000),audio+100000);
        QCOMPARE(clock.schedule("peer-2",90000,{1,90000},6000000),qint64(6060000));
        clock.reset("peer-1");QCOMPARE(clock.schedule("peer-1",0,{1,1000000},7000000),qint64(7060000));
    }
    void sharedSlotOwnership() {
        media::detail::SharedFrames writer, reader;
        QVERIFY(writer.create(42));
        QVERIFY(reader.open(writer.key(), 42));
        QByteArray bytes(128, 'a');
        int a = writer.publish(reinterpret_cast<const uchar *>(bytes.constData()), bytes.size(),1);
        QVERIFY(a >= 0);
        const auto *p = reader.acquire(a, 128,1);
        QVERIFY(p);
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(p), 128), bytes);
        int b = writer.publish(reinterpret_cast<const uchar *>(bytes.constData()), 128,2),
            c = writer.publish(reinterpret_cast<const uchar *>(bytes.constData()), 128,3);
        QVERIFY(b >= 0);
        QVERIFY(c >= 0);
        QCOMPARE(writer.publish(reinterpret_cast<const uchar *>(bytes.constData()),128,4),b);
        QVERIFY(!reader.acquire(b,128,2));QVERIFY(reader.acquire(b,128,4));QVERIFY(reader.acquire(c,128,3));
        QCOMPARE(writer.publish(reinterpret_cast<const uchar *>(bytes.constData()),128,5),-1);
        reader.release(a);
        QCOMPARE(writer.publish(reinterpret_cast<const uchar *>(bytes.constData()),128,5),a);
        reader.release(b);reader.release(c);
        media::detail::SharedFrames stale;
        QVERIFY(!stale.open(writer.key(), 43));
    }
    void bulkFragmentationAndBounds() {
        protocol::service::BulkChannel server, client;
        auto descriptor = server.listen(QString(32, 'a'), "42");
        QVERIFY(!descriptor.isEmpty());
        client.open(descriptor);
        QTRY_VERIFY_WITH_TIMEOUT(client.ready() && server.ready(), 5000);
        QByteArray input(600000, 'x'), output;
        server.received = [&](const QJsonObject &h, const QByteArray &p) {
            QCOMPARE(h["kind"].toString(), QString("large"));
            output = p;
        };
        QVERIFY(client.send({{"kind", "large"}}, input));
        QTRY_COMPARE(output, input);
        QVERIFY(!client.send({}, QByteArray(protocol::service::BulkChannel::MessageBytes + 1, 'x')));
    }
    void bulkRejectsWrongCredential() {
        protocol::service::BulkChannel server, client;
        auto d = server.listen(QString(32, 'a'), "1");
        d["token"] = QString(64, 'b');
        bool failed = false;
        server.failed = [&](const QString &) { failed = true; };
        client.open(d);
        QTRY_VERIFY(failed);
        QVERIFY(!server.ready());
    }
    void bulkReceiverCanDestroyItself() {
        protocol::service::BulkChannel server;
        auto client = std::make_unique<protocol::service::BulkChannel>();
        auto descriptor = server.listen(QString(32, 'a'), "9");
        client->open(descriptor);
        QTRY_VERIFY(client->ready() && server.ready());
        client->received = [&](const QJsonObject &, const QByteArray &) { client.reset(); };
        QVERIFY(server.send({{"type", "self-delete"}}, "payload"));
        QTRY_VERIFY(!client);
    }
    void bulkOutputBackpressureIsBounded() {
        protocol::service::BulkChannel server,client;auto descriptor=server.listen(QString(32,'b'),"7");client.open(descriptor);QTRY_VERIFY(server.ready()&&client.ready());
        QByteArray bytes(protocol::service::BulkChannel::MessageBytes,'x');int accepted=0;
        while(accepted<4&&client.send({},bytes))++accepted;
        QVERIFY(accepted>0);QVERIFY(accepted<4);
    }
    void resamplingAndOpusPlc() {
        media::Resampler r;
        QVector<float> output;
        for (int i = 0; i < 20; ++i)
            output += r.process(QVector<float>(441, .2f), 44100, 48000);
        QVERIFY(qAbs(output.size() - 9600) < 64);
        int error = 0;
        auto *encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error);
        QVERIFY(encoder);
        media::JitterStream stream;
        for (int j = 0; j < 10; ++j) {
            QVector<float> samples(media::FrameSamples, .1f);
            QByteArray p(media::MaxOpusBytes, Qt::Uninitialized);
            int n = opus_encode_float(encoder, samples.constData(), media::FrameSamples,
                                      reinterpret_cast<unsigned char *>(p.data()), int(p.size()));
            QVERIFY(n > 0);
            p.resize(n);
            if (j != 4)
                stream.push(quint32(j), p);
        }
        for (int j = 0; j < 15; ++j)
            for (float v : stream.pull())
                QVERIFY(std::isfinite(v));
        opus_encoder_destroy(encoder);
    }
    void h264AndIndependentExtensionInstances() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(QDir(root.path()).mkpath("extensions/flip"));
        const auto extension=qEnvironmentVariable("ZTO_MEDIA_EXTENSION_PATH",QString::fromUtf8(MEDIA_FLIP));
        QVERIFY(QFile::copy(extension, root.path() + "/extensions/flip/flip.dll"));
        QFile manifest(root.path() + "/extensions.json");
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(QJsonDocument(QJsonArray{QJsonObject{{"path", "extensions/flip/flip.dll"},
                                                            {"abi", 1},
                                                            {"ffmpeg", ZTMEDIA_FFMPEG_FINGERPRINT}}})
                           .toJson());
        manifest.close();
        media::engine::ExtensionRegistry registry;
        QVERIFY2(registry.load(root.path()).isEmpty(), "Extension failed to load");
        media::TrackConfig config;
        config.kind = "video";
        config.source = "synthetic";
        config.video.width = 320;
        config.video.height = 180;
        config.codec.codec = "h264";
        config.codec.bitrate = 500000;
        config.processors = {"example.hflip"};
        media::engine::VideoCodec a, b, decoder;
        QVERIFY(a.open(config, registry).isEmpty());
        QVERIFY(b.open(config, registry).isEmpty());
        config.direction = "receive";
        config.processors.clear();
        QVERIFY(decoder.open(config, registry).isEmpty());
        QImage image(320, 180, QImage::Format_ARGB32);
        image.fill(Qt::blue);
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 100; ++x)
                image.setPixelColor(x, y, Qt::red);
        QString error;
        int decoded = 0;
        for (int i = 0; i < 8; ++i) {
            auto packets = a.encode(image, i * 33333, error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            for (auto &p : packets) {
                auto frames = decoder.decode(p, error);
                QVERIFY2(error.isEmpty(), qPrintable(error));
                for (const auto &f : frames) {
                    ++decoded;
                    QVERIFY(f.first.pixelColor(300, 90).red() > 180);
                    QVERIFY(f.first.pixelColor(10, 90).blue() > 180);
                }
            }
        }
        QVERIFY(decoded > 0);
        auto packets = b.encode(image, 0, error);
        QVERIFY(!packets.isEmpty());
        QVERIFY(packets.first().keyFrame);
        QCOMPARE(packets.first().sequence, quint64(0));
    }
    void rejectExtensionFingerprint() {
        QTemporaryDir root;
        QFile manifest(root.path() + "/extensions.json");
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write("[{\"path\":\"extensions/no.dll\",\"abi\":1,\"ffmpeg\":\"wrong\"}]");
        manifest.close();
        media::engine::ExtensionRegistry registry;
        QVERIFY(registry.load(root.path()).contains("fingerprint"));
    }
};
QTEST_MAIN(MediaEngineTests)
#include "MediaEngineTests.moc"
