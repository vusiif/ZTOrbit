#include "../../client/main/plugins/ServiceBroker.h"
#include "PluginPackage.h"
#include <ztmedia/MediaSession.h>
#include <QtTest>
#include <QTemporaryDir>
#include <QWindow>
#include <QScreen>
#include <QImage>
#include <QFile>
#include <QWidget>
#include <QPainter>
#include <QScopeGuard>
#include <QPointer>
#include <QtEndian>
#include <chrono>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif
using namespace vusiif::zto;
namespace {
QByteArray clipWav(int frames)
{
    QByteArray body;
    QByteArray fmt(16, '\0');
    qToLittleEndian(quint16(1), reinterpret_cast<uchar *>(fmt.data()));
    qToLittleEndian(quint16(1), reinterpret_cast<uchar *>(fmt.data() + 2));
    qToLittleEndian(quint32(48000), reinterpret_cast<uchar *>(fmt.data() + 4));
    qToLittleEndian(quint32(96000), reinterpret_cast<uchar *>(fmt.data() + 8));
    qToLittleEndian(quint16(2), reinterpret_cast<uchar *>(fmt.data() + 12));
    qToLittleEndian(quint16(16), reinterpret_cast<uchar *>(fmt.data() + 14));
    body += "fmt ";
    QByteArray sizes(4, '\0');
    qToLittleEndian(quint32(16), reinterpret_cast<uchar *>(sizes.data()));
    body += sizes + fmt;
    body += "data";
    qToLittleEndian(quint32(frames * 2), reinterpret_cast<uchar *>(sizes.data()));
    body += sizes;
    body += QByteArray(frames * 2, '\0');
    QByteArray wav("RIFF");
    QByteArray riff(4, '\0');
    qToLittleEndian(quint32(body.size() + 4), reinterpret_cast<uchar *>(riff.data()));
    wav += riff + "WAVE" + body;
    return wav;
}
}
class CaptureFixtureWindow final : public QWidget {
    int frames = 0;
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(QRect(0, 0, width(), height()), QColor::fromHsv(frames++ % 360, 200, 180));
    }
};
class MediaIntegrationTests : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> cache;
    std::unique_ptr<QLockFile> lock;
    std::unique_ptr<plugins::ServiceBroker> broker;
    QTemporaryDir clipFailureDirectory;
    QString clipFailureFile;
    template <class T, class F> media::Result<T> await(F operation, int timeout = 10000) {
        QObject scope;
        media::Result<T> result;
        bool complete = false;
        operation(&scope, [&](media::Result<T> r) {
            result = std::move(r);
            complete = true;
        });
        QElapsedTimer timer;
        timer.start();
        while (!complete && timer.elapsed() < timeout)
            QTest::qWait(5);
        if (!complete)
            result.error = {"timeout", "Operation timed out"};
        return result;
    }
    std::unique_ptr<media::MediaSession> consumer() {
        return std::make_unique<media::MediaSession>(
            broker->acquire(media::SessionService, media::SessionApi));
    }
    static media::TrackConfig video(QString source = "synthetic") {
        media::TrackConfig c;
        c.kind = "video";
        c.source = source;
        c.preview = true;
        c.codec.codec = "h264";
        c.codec.bitrate = 2000000;
        c.codec.allowAdjustment = true;
        return c;
    }
    media::Result<media::TrackDescriptor> create(media::MediaSession &s, const media::TrackConfig &c) {
        return await<media::TrackDescriptor>([&](auto *ctx, auto cb) { s.createTrack(c, ctx, cb); });
    }
    bool start(media::MediaSession &s, const QString &id) {
        return await<bool>([&](auto *ctx, auto cb) { s.start(id, ctx, cb); }).ok();
    }
    bool close(media::MediaSession &s) {
        return await<bool>([&](auto *ctx, auto cb) { s.close(ctx, cb); }).ok();
    }
  private slots:
    void initTestCase() {
        QVERIFY(clipFailureDirectory.isValid());
        clipFailureFile = clipFailureDirectory.filePath("clip-failure.txt");
        // The worker inherits this environment at start; the file is created
        // only by the failure test. Production never sets this variable.
        qputenv("ZTO_MEDIA_TEST_CLIP_FAILURE", clipFailureFile.toUtf8());
    }
    void init() {
        QFile::remove(clipFailureFile);
        auto package = package::inspect(qEnvironmentVariable("ZTO_TEST_MEDIA_PACKAGE",QString::fromUtf8(MEDIA_PLUGIN)));
        QVERIFY2(package.ok(), qPrintable(package.error));
        QString entry;
        auto error = package::extract(package, cache, entry, lock);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        broker = std::make_unique<plugins::ServiceBroker>();
        broker->start(entry, cache->path(), package.info);
        QTRY_VERIFY_WITH_TIMEOUT(broker->ready() || !broker->failure().isEmpty(), 80000);
        QVERIFY2(broker->ready(), qPrintable(broker->failure()));
    }
    void cleanup() {
        if (broker) {
            broker->stop();
            QTRY_VERIFY_WITH_TIMEOUT(broker->exited(), 10000);
            QVERIFY(!broker->forced());
            broker.reset();
        }
        lock.reset();
        package::discardCache(cache);
    }
    void syntheticRoundtripAndIsolation() {
        auto a = consumer(), b = consumer();
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { a->open(ctx, cb); }).ok());
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { b->open(ctx, cb); }).ok());
        auto send = create(*a, video());
        QVERIFY2(send.ok(), qPrintable(send.error.message));
        auto c = video();
        c.direction = "receive";
        c.source.clear();
        auto receive = create(*a, c);
        QVERIFY(receive.ok());
        auto independent = create(*b, video());
        QVERIFY(independent.ok());
        int decoded = 0, raw = 0, other = 0;
        QStringList errors;
        a->error = [&](const media::MediaError &e) { errors.append(e.message); };
        a->packet = [&](media::EncodedPacket p) {
            p.track = receive.value.id;
            QVERIFY(a->submit(std::move(p)));
        };
        a->frame = [&](const media::FrameView &f) {
            QVERIFY(f.data);
            if (f.decoded)
                ++decoded;
            else
                ++raw;
        };
        b->frame = [&](const media::FrameView &) { ++other; };
        // On an assertion failure callbacks must stop before their captured
        // stack counters disappear; otherwise the failure path corrupts the
        // heap and hides the original stream diagnostics.
        const auto detachCallbacks = qScopeGuard([&] {
            a->packet = {}; a->frame = {}; a->error = {}; b->frame = {};
            close(*a); close(*b);
        });
        QVERIFY(start(*a, receive.value.id));
        QVERIFY(start(*a, send.value.id));
        QVERIFY(start(*b, independent.value.id));
        QElapsedTimer progress;
        progress.start();
        while ((decoded < 20 || other < 20) && progress.elapsed() < 8000) QTest::qWait(10);
        if (decoded < 20 || other < 20) {
            const auto first = await<media::Statistics>([&](auto *ctx, auto cb) { a->statistics(ctx, cb); });
            const auto second = await<media::Statistics>([&](auto *ctx, auto cb) { b->statistics(ctx, cb); });
            qInfo() << "roundtrip counters" << decoded << raw << other << errors
                    << QJsonDocument(first.value.values).toJson(QJsonDocument::Compact)
                    << QJsonDocument(second.value.values).toJson(QJsonDocument::Compact);
        }
        QVERIFY(decoded >= 20 && other >= 20);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(";")));
        QVERIFY(raw >= decoded);
        QVERIFY(close(*a));
        int before = other;
        QTRY_VERIFY(other > before + 5);
        QVERIFY(close(*b));
        QTRY_VERIFY(broker->idle());
#ifdef Q_OS_WIN
        auto snapshot =
            CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        QVERIFY(snapshot != INVALID_HANDLE_VALUE);
        MODULEENTRY32W module{};
        module.dwSize = sizeof module;
        if (Module32FirstW(snapshot, &module))
            do {
                auto name = QString::fromWCharArray(module.szModule).toLower();
                QVERIFY2(!name.startsWith("avcodec-") && !name.startsWith("avfilter-") &&
                             name != "qt6multimedia.dll" && name != "z2media_flip.dll",
                         qPrintable(name));
            } while (Module32NextW(snapshot, &module));
        CloseHandle(snapshot);
#endif
    }
    void tileConfigurationRejectsInitializationData() {
        auto s = consumer();
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        const auto cleanup = qScopeGuard([&] { if (!broker->idle()) close(*s); });
        auto config = video();
        config.codec.codec = "bgra-tiles";
        config.video.colorRange = "full";
        config.preview = false;
        for (const auto &direction : {QStringLiteral("send"), QStringLiteral("receive")}) {
            config.direction = direction;
            config.source = direction == "send" ? "synthetic" : "";
            config.codec.initializationData = "unexpected";
            const auto invalid = create(*s, config);
            QVERIFY2(!invalid.ok(), "Tile initialization data must be rejected");
            QVERIFY(invalid.error.message.contains("initialization data"));
            config.codec.initializationData.clear();
            const auto valid = create(*s, config);
            QVERIFY2(valid.ok(), qPrintable(valid.error.message));
            config.codec.initializationData = "unexpected";
            const auto configured = await<media::TrackDescriptor>([&](auto *ctx, auto cb) {
                s->configure(valid.value.id, config, ctx, cb);
            });
            QVERIFY2(!configured.ok(), "Reconfiguration must enforce the same tile contract");
            QVERIFY(configured.error.message.contains("initialization data"));
        }
        QVERIFY(close(*s));
        QTRY_VERIFY(broker->idle());
    }
    void delayedFirstPacketDoesNotStarveIndependentVideo() {
        auto s = consumer();
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        auto config = video();
        config.video.width = 320;
        config.video.height = 180;
        config.codec.keyFrameInterval = 1; // Every retained packet is independently decodable.
        config.preview = false;
        const auto tx = create(*s, config);
        QVERIFY(tx.ok());
        config.direction = "receive";
        config.source.clear();
        config.preview = true;
        const auto rx = create(*s, config);
        QVERIFY(rx.ok());
        int decoded = 0;
        bool haveFirst = false, live = false;
        media::EncodedPacket first;
        QTimer delayed;
        delayed.setSingleShot(true);
        const auto callbacks = qScopeGuard([&] {
            delayed.stop();
            s->packet = {};
            s->frame = {};
            if (!broker->idle()) close(*s);
        });
        QObject::connect(&delayed, &QTimer::timeout, this, [&] {
            QVERIFY(s->submitTo(rx.value, std::move(first)));
            live = true;
        });
        s->packet = [&](media::EncodedPacket packet) {
            if (!haveFirst) {
                first = std::move(packet);
                haveFirst = true;
                delayed.start(500);
            } else if (live) {
                QVERIFY(s->submitTo(rx.value, std::move(packet)));
            }
        };
        s->frame = [&](const media::FrameView &frame) {
            if (frame.track == rx.value.id && frame.decoded) ++decoded;
        };
        QVERIFY(start(*s, rx.value.id));
        QVERIFY(start(*s, tx.value.id));
        QTest::qWait(3500);
        const auto stats = await<media::Statistics>([&](auto *ctx, auto cb) { s->statistics(ctx, cb); });
        QVERIFY(stats.ok());
        const auto track = stats.value.track(rx.value.id);
        qInfo().noquote() << "delayed first packet" << "decoded" << decoded
            << QJsonDocument(track).toJson(QJsonDocument::Compact);
        QVERIFY2(decoded >= 10, "An old first packet must not starve all subsequent live video");
        QVERIFY(track["decodedQueuePeak"].toString().toULongLong() <= 3);
        QVERIFY(track["playbackReanchors"].toString().toULongLong() >= 1);
        const int before = decoded;
        QTest::qWait(500);
        QVERIFY(decoded > before);
        QVERIFY(close(*s));
        QTRY_VERIFY(broker->idle());
    }
    void clipServiceAcceptsAndFinishesShortAudio() {
        auto lease = broker->acquire(media::ClipService, media::ClipApi);
        QVERIFY2(lease, "media.clip lease unavailable");
        QObject scope;
        bool done = false;
        QString error;
        lease->call("play", {{"device", ""}, {"gain", 1.0}}, QByteArray("not a wav"), &scope,
                    [&](const util::ServiceResult &result) { done = true; error = result.error; });
        QTRY_VERIFY(done);
        QVERIFY(!error.isEmpty()); // Validation happens before playback.

        bool accepted = false, finished = false, failed = false;
        QString clipId, failure;
        lease->subscribe(&scope, [&](const util::ServiceResult &result) {
            const auto event = result.data["event"].toString();
            if (event == "finished") finished = true;
            else if (event == "failed") { failed = true; failure = result.data["message"].toString(); }
        });
        done = false; error.clear();
        lease->call("play", {{"device", ""}, {"gain", 0.5}}, clipWav(7680), &scope,
                    [&](const util::ServiceResult &result) {
                        done = true; accepted = result.ok(); error = result.error;
                        clipId = result.data["clipId"].toString();
                    });
        QTRY_VERIFY(done);
        if (!accepted && error.contains("unavailable"))
            QSKIP("No physical audio output is available for clip playback");
        QVERIFY2(accepted, qPrintable(error));
        QVERIFY(!clipId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(finished || failed, 5000);
        QVERIFY2(finished, qPrintable(failure));
        bool stopped = false;
        done = false;
        lease->call("stop", {{"clipId", clipId}}, {}, &scope,
                    [&](const util::ServiceResult &result) { done = true; stopped = result.ok(); });
        QTRY_VERIFY(done); QVERIFY(stopped);
        done = false;
        lease->call("stop", {{"clipId", "unknown-clip"}}, {}, &scope,
                    [&](const util::ServiceResult &result) { done = true; stopped = result.ok(); });
        QTRY_VERIFY(done); QVERIFY(stopped);
        done = false;
        lease->close(&scope, [&](const util::ServiceResult &result) { done = true; stopped = result.ok(); });
        QTRY_VERIFY(done); QVERIFY(stopped);
    }

    void clipServiceReportsFailureAfterAcceptance()
    {
        auto lease = broker->acquire(media::ClipService, media::ClipApi);
        QVERIFY2(lease, "media.clip lease unavailable");
        QObject scope;
        bool finished = false, failed = false;
        QString failure;
        lease->subscribe(&scope, [&](const util::ServiceResult &result) {
            const auto event = result.data["event"].toString();
            if (event == "finished") finished = true;
            else if (event == "failed") { failed = true; failure = result.data["message"].toString(); }
        });
        {
            QFile injection(clipFailureFile);
            QVERIFY(injection.open(QIODevice::WriteOnly));
            // Accept two 20 ms chunks, then reject the third one.
            injection.write("2 nonce-1");
        }
        bool done = false, accepted = false;
        QString error, clipId;
        lease->call("play", {{"device", ""}, {"gain", 1.0}}, clipWav(7680), &scope,
                    [&](const util::ServiceResult &result) {
                        done = true; accepted = result.ok(); error = result.error;
                        clipId = result.data["clipId"].toString();
                    });
        QTRY_VERIFY(done);
        if (!accepted && error.contains("unavailable"))
            QSKIP("No physical audio output is available for clip playback");
        QVERIFY2(accepted, qPrintable(error));
        QVERIFY(!clipId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(finished || failed, 5000);
        QVERIFY2(failed, "The injected output failure must emit failed");
        QVERIFY(!finished);
        QVERIFY(!failure.isEmpty());
        // The lease stays usable and later clips still reach a terminal event.
        QVERIFY(QFile::remove(clipFailureFile));
        finished = failed = false;
        done = false; accepted = false;
        lease->call("play", {{"device", ""}, {"gain", 1.0}}, clipWav(7680), &scope,
                    [&](const util::ServiceResult &result) {
                        done = true; accepted = result.ok(); error = result.error;
                    });
        QTRY_VERIFY(done);
        QVERIFY2(accepted, qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(finished || failed, 5000);
        QVERIFY2(finished, qPrintable(failure));
        bool closed = false;
        lease->close(&scope, [&](const util::ServiceResult &result) { closed = result.ok(); });
        QTRY_VERIFY(closed);
    }

    void closeInsideFrameCallback() {
        auto s = consumer();
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        auto track = create(*s, video());
        QVERIFY(track.ok());
        bool closed = false;
        s->frame = [&](const media::FrameView &f) {
            QVERIFY(f.data);
            s->close(this, [&](media::Result<bool> r) {
                QVERIFY(r.ok());
                closed = true;
            });
        };
        QVERIFY(start(*s, track.value.id));
        QTRY_VERIFY_WITH_TIMEOUT(closed, 10000);
        QTRY_VERIFY(broker->idle());
    }
    void workerExtensionRoundtrip() {
        auto s=consumer();QVERIFY(await<bool>([&](auto *ctx,auto cb){s->open(ctx,cb);}).ok());
        auto config=video();config.processors={"example.hflip"};auto tx=create(*s,config);QVERIFY2(tx.ok(),qPrintable(tx.error.message));config.direction="receive";config.processors.clear();auto rx=create(*s,config);QVERIFY(rx.ok());int verified=0;
        s->packet=[&](media::EncodedPacket p){QVERIFY(s->submitTo(rx.value,std::move(p)));};
        s->frame=[&](const media::FrameView &f){if(!f.decoded)return;const auto *pixel=f.data+f.planes[0].stride*(f.video.height/2)+(f.video.width-10)*4;QVERIFY(pixel[2]>180&&pixel[0]<100);++verified;};
        QVERIFY(start(*s,rx.value.id));QVERIFY(start(*s,tx.value.id));QTRY_VERIFY_WITH_TIMEOUT(verified>=5,10000);QVERIFY(close(*s));
    }
    void destroySessionInsideFrameCallback() {
        auto s = consumer();
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        auto track = create(*s, video());
        QVERIFY(track.ok());
        s->frame = [&](const media::FrameView &f) {
            QVERIFY(f.data);
            s.reset();
        };
        QVERIFY(start(*s, track.value.id));
        QTRY_VERIFY_WITH_TIMEOUT(!s, 10000);
        QTRY_VERIFY(broker->idle());
    }
    void workerCrashInvalidatesLease() {
#ifdef Q_OS_WIN
        auto s = consumer();
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(broker->pid()));
        QVERIFY(process);
        QVERIFY(TerminateProcess(process, 71));
        CloseHandle(process);
        QTRY_VERIFY(!broker->failure().isEmpty());
        auto result = await<media::Capabilities>([&](auto *ctx, auto cb) { s->capabilities(ctx, cb); });
        QVERIFY(!result.ok());
        QVERIFY(close(*s));
#endif
    }
    void slowConsumerKeepsBuffersBounded() {
        auto s=consumer();QVERIFY(await<bool>([&](auto *ctx,auto cb){s->open(ctx,cb);}).ok());auto tx=create(*s,video());QVERIFY(tx.ok());
        int callbacks=0;s->frame=[&](const media::FrameView &){++callbacks;QThread::msleep(80);};QVERIFY(start(*s,tx.value.id));QElapsedTimer timer;timer.start();while(timer.elapsed()<2000)QTest::qWait(10);
        auto stats=await<media::Statistics>([&](auto *ctx,auto cb){s->statistics(ctx,cb);});QVERIFY(stats.ok());QVERIFY(stats.value.values["queuePeak"].toInt()<=32);QVERIFY(callbacks>0);
        bool dropped=false;for(const auto &row:stats.value.values["tracks"].toArray())dropped|=row.toObject()["dropped"].toString().toULongLong()>0;
        QVERIFY(close(*s));QVERIFY(dropped);
    }
    void syntheticProfilesLoopback_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<int>("bitrate");
        QTest::newRow("720p30") << 1280 << 720 << 2000000;
        QTest::newRow("1080p30") << 1920 << 1080 << 4000000;
    }
    void syntheticProfilesLoopback() {
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(int, bitrate);
        auto s = consumer();
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        auto c = video();
        c.video.width = width;
        c.video.height = height;
        c.codec.bitrate = bitrate;
        c.codec.keyFrameInterval = 30;
        auto tx = create(*s, c);
        QVERIFY2(tx.ok(), qPrintable(tx.error.message));
        c.direction = "receive";
        c.source.clear();
        auto rx = create(*s, c);
        QVERIFY(rx.ok());
        int packets = 0, keyFrames = 0, decoded = 0;
        qint64 lastDuration = 0;
        QSize decodedSize;
        QList<quint64> sequences;
        s->packet = [&](media::EncodedPacket p) {
            ++packets;
            sequences.append(p.sequence);
            lastDuration = p.duration;
            if (p.keyFrame)
                ++keyFrames;
            QVERIFY(s->submitTo(rx.value, std::move(p)));
        };
        s->frame = [&](const media::FrameView &f) {
            if (!f.decoded)
                return;
            ++decoded;
            decodedSize = QSize(f.video.width, f.video.height);
        };
        QVERIFY(start(*s, rx.value.id));
        QVERIFY(start(*s, tx.value.id));
        QTRY_VERIFY_WITH_TIMEOUT(decoded >= 10, 10000);
        QCOMPARE(decodedSize, QSize(width, height));
        QCOMPARE(lastDuration, qint64(1000000 / 30));
        // The first packet is a key frame; a key frame request must produce a
        // later one instead of silently keeping the previous picture.
        QVERIFY(keyFrames >= 1);
        const auto beforeRequest = keyFrames;
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->requestKeyFrame(tx.value.id, ctx, cb); }).ok());
        QTest::qWait(200);
        QTRY_VERIFY_WITH_TIMEOUT(keyFrames > beforeRequest, 3000);
        for (qsizetype i = 1; i < sequences.size(); ++i)
            QVERIFY(sequences[i] > sequences[i - 1]);
        QVERIFY(close(*s));
        QTRY_VERIFY(broker->idle());
    }

    void syntheticAndCachedRefreshSharePacingBudget() {
        auto s = consumer();
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        auto config = video();
        config.video.width = 320; config.video.height = 180;
        config.codec.frameRate = 30; config.preview = false;
        auto tx = create(*s, config);
        QVERIFY(tx.ok());
        int packets = 0;
        const auto callbacks = qScopeGuard([&] { s->packet = {}; close(*s); });
        s->packet = [&](media::EncodedPacket) { ++packets; };
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(start(*s, tx.value.id));
        while (elapsed.elapsed() < 2500) {
            QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->requestKeyFrame(tx.value.id, ctx, cb); }).ok());
            QTest::qWait(10);
        }
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->stop(tx.value.id, ctx, cb); }).ok());
        QVERIFY2(packets <= 2 + (elapsed.elapsed() * 30 + 999) / 1000,
                 qPrintable(QStringLiteral("pacing budget exceeded: %1 in %2ms").arg(packets).arg(elapsed.elapsed())));
        QVERIFY(packets >= 60);
        qInfo() << "synthetic+keyframe packets" << packets << "elapsedMs" << elapsed.elapsed();
    }
    void keyFrameRequestRefreshesCachedCanvas() {
        auto s = consumer();
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        auto config = video();
        config.video.width = 320;
        config.video.height = 180;
        config.codec.bitrate = 500000;
        config.codec.frameRate = 1;
        config.preview = false;
        const auto tx = create(*s, config);
        QVERIFY(tx.ok());
        config.direction = "receive";
        config.source.clear();
        config.preview = true;
        const auto rx = create(*s, config);
        QVERIFY(rx.ok());
        int frames = 0, keyFrames = 0;
        const auto callbacks = qScopeGuard([&] { s->packet = {}; s->frame = {}; if(!broker->idle())close(*s); });
        s->packet = [&](media::EncodedPacket packet) {
            if (packet.keyFrame) ++keyFrames;
            QVERIFY(s->submitTo(rx.value, std::move(packet)));
        };
        s->frame = [&](const media::FrameView &frame) { if (frame.decoded) ++frames; };
        QVERIFY(start(*s, rx.value.id));
        QVERIFY(start(*s, tx.value.id));
        QTRY_VERIFY_WITH_TIMEOUT(frames >= 1, 3000);
        const auto before = await<media::Statistics>([&](auto *ctx, auto cb) { s->statistics(ctx, cb); });
        QVERIFY(before.ok());
        const auto captured = before.value.track(tx.value.id)["captured"].toString();
        const auto previousFrames = frames, previousKeyFrames = keyFrames;
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->requestKeyFrame(tx.value.id, ctx, cb); }).ok());
        // The source is only 1 fps; this must not wait for its next input.
        QTRY_VERIFY_WITH_TIMEOUT(frames > previousFrames && keyFrames > previousKeyFrames, 500);
        const auto after = await<media::Statistics>([&](auto *ctx, auto cb) { s->statistics(ctx, cb); });
        QVERIFY(after.ok());
        const auto stats = after.value.track(tx.value.id);
        QCOMPARE(stats["captured"].toString(), captured);
        QCOMPARE(stats["keyFrameRefreshes"].toString(), QStringLiteral("1"));
        QVERIFY(after.value.track(QStringLiteral("missing-track")).isEmpty());
        qInfo().noquote() << "cached keyframe refresh" << QJsonDocument(stats).toJson(QJsonDocument::Compact);
        QVERIFY(close(*s));
        QTRY_VERIFY(broker->idle());
    }

    void physical1080pLoopback_data() {
        QTest::addColumn<bool>("preview");
        QTest::newRow("preview") << true;
        QTest::newRow("decode-only") << false;
    }
    void physical1080pLoopback() {
        if (!qEnvironmentVariableIntValue("ZTO_TEST_PHYSICAL_VIDEO"))
            QSKIP("Set ZTO_TEST_PHYSICAL_VIDEO=1 for real window capture");
        QFETCH(bool, preview);
        CaptureFixtureWindow window;
        window.setWindowTitle("ZTOrbit 1080p loopback fixture");
        window.resize(360, 240);
        window.move(1450, 850);
        window.show();
        QTest::qWait(100);
        window.hide();
        window.show();
        window.raise();
        QTimer animation;
        QObject::connect(&animation, &QTimer::timeout, &window, [&window] { window.update(); });
        animation.start(33);
        QTest::qWait(400);
        auto s = consumer();
        const auto sources = await<media::SourceSnapshot>([&](auto *ctx, auto cb) { s->sources(ctx, cb); });
        QVERIFY(sources.ok());
        QString source;
        for (const auto &entry : sources.value.sources)
            if (entry.kind == "window" && entry.description.contains(window.windowTitle()))
                source = entry.kind + ":" + entry.id;
        if (source.isEmpty()) {
            QVERIFY(close(*s));
            QSKIP("Fixture window is not capturable on this platform");
        }
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        auto config = video(source);
        config.video.width = 1920;
        config.video.height = 1080;
        config.codec.bitrate = 4000000;
        config.codec.keyFrameInterval = 30;
        config.preview = false;
        const auto tx = create(*s, config);
        QVERIFY2(tx.ok(), qPrintable(tx.error.message));
        config.direction = "receive";
        config.source.clear();
        config.preview = preview;
        const auto rx = create(*s, config);
        QVERIFY2(rx.ok(), qPrintable(rx.error.message));
        int packets = 0, frames = 0, submitDrops = 0;
        QString failure;
        const auto callbacks = qScopeGuard([&] { s->packet = {}; s->frame = {}; s->error = {}; if(!broker->idle())close(*s); });
        s->error = [&](const media::MediaError &error) { failure = error.message; };
        s->packet = [&](media::EncodedPacket packet) {
            ++packets;
            if (!s->submitTo(rx.value, std::move(packet))) ++submitDrops;
        };
        s->frame = [&](const media::FrameView &frame) {
            if (frame.track == rx.value.id && frame.decoded) {
                ++frames;
                QCOMPARE(QSize(frame.video.width, frame.video.height), QSize(1920, 1080));
            }
        };
        QVERIFY(start(*s, rx.value.id));
        QVERIFY(start(*s, tx.value.id));
        QJsonObject receiveStats;
        int framesBeforeLastInterval = 0;
        for (int interval = 0; interval < 5; ++interval) {
            if (interval == 4) framesBeforeLastInterval = frames;
            QTest::qWait(2000);
            const auto stats = await<media::Statistics>([&](auto *ctx, auto cb) { s->statistics(ctx, cb); });
            QVERIFY(stats.ok());
            receiveStats = stats.value.track(rx.value.id);
            qInfo().noquote() << "1080p receive statistics"
                              << QJsonDocument(receiveStats).toJson(QJsonDocument::Compact);
        }
        qInfo() << "1080p ten-second loopback" << "preview" << preview << "packets" << packets
                << "frames" << frames << "submitDrops" << submitDrops;
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY(receiveStats["decodedOutput"].toString().toULongLong() >= 10);
        if (preview) {
            QVERIFY(frames >= 10);
            QVERIFY2(frames > framesBeforeLastInterval, "preview stopped progressing in the final interval");
        } else {
            QCOMPARE(frames, 0);
            QCOMPARE(receiveStats["frames"].toString(), QStringLiteral("0"));
        }
        QVERIFY(close(*s));
        QTRY_VERIFY(broker->idle());
    }

    void physicalAspectLoopback() {
        if (!qEnvironmentVariableIntValue("ZTO_TEST_PHYSICAL_VIDEO"))
            QSKIP("Set ZTO_TEST_PHYSICAL_VIDEO=1 for device capture");
        CaptureFixtureWindow window;
        window.setWindowTitle("ZTOrbit aspect fixture");
        window.resize(500, 420);
        // The same show/hide/show sequence used by the existing window capture
        // test; it makes the fixture reliably appear in the capturable list.
        window.show();
        QTest::qWait(100);
        window.hide();
        window.show();
        QTimer animation;
        QObject::connect(&animation, &QTimer::timeout, &window, [&window] { window.update(); });
        animation.start(33);
        QTest::qWait(400);
        auto s = consumer();
        auto sources = await<media::SourceSnapshot>([&](auto *ctx, auto cb) { s->sources(ctx, cb); });
        QVERIFY(sources.ok());
        QString source;
        for (const auto &d : sources.value.sources)
            if (d.kind == "window" && d.description.contains(window.windowTitle())) {
                source = d.kind + ":" + d.id;
                break;
            }
        if (source.isEmpty()) {
            QStringList windows;
            for (const auto &d : sources.value.sources)
                if (d.kind == "window")
                    windows.append(d.description);
            qWarning() << "Capturable windows:" << windows;
            QVERIFY(close(*s));
            QSKIP("Fixture window is not capturable on this platform");
        }
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        auto tx = create(*s, video(source));
        QVERIFY2(tx.ok(), qPrintable(tx.error.message));
        auto c = video();
        c.direction = "receive";
        c.source.clear();
        auto rx = create(*s, c);
        QVERIFY(rx.ok());
        int decoded = 0, raw = 0;
        QString deviceError;
        QImage last;
        const auto callbacks = qScopeGuard([&] { s->packet = {}; s->frame = {}; s->error = {}; if(!broker->idle())close(*s); });
        s->error = [&](const media::MediaError &e) {
            if (e.code == QStringLiteral("device"))
                deviceError = e.message;
            qWarning() << e.code << e.message;
        };
        s->packet = [&](media::EncodedPacket p) { QVERIFY(s->submitTo(rx.value, std::move(p))); };
        s->frame = [&](const media::FrameView &f) {
            if (!f.decoded) {
                ++raw;
                return;
            }
            ++decoded;
            last = QImage(f.data, f.video.width, f.video.height, f.planes.front().stride,
                          QImage::Format_ARGB32)
                       .copy();
        };
        QVERIFY(start(*s, rx.value.id));
        QVERIFY(start(*s, tx.value.id));
        QElapsedTimer wait;
        wait.start();
        while (decoded < 20 && wait.elapsed() < 15000)
            QTest::qWait(20);
        QVERIFY2(decoded >= 20, qPrintable(QStringLiteral("decoded %1").arg(decoded)));
        QVERIFY(raw > 0);
        animation.stop();
        QCOMPARE(last.size(), QSize(1280, 720));
        // A 500x420 window is narrower than 16:9, so the fixed canvas keeps the
        // window shape and fills the sides with black instead of stretching.
        QVERIFY2(last.pixelColor(2, 360).lightness() < 30, "left edge must be a black bar");
        QVERIFY2(last.pixelColor(1277, 360).lightness() < 30, "right edge must be a black bar");
        QVERIFY2(last.pixelColor(640, 360).lightness() > 40, "center must keep the window content");
        // A static window (no repaint) is normal content: it must not be
        // reported as a source failure.
        QTest::qWait(4000);
        QVERIFY2(deviceError.isEmpty(), qPrintable(deviceError));
        const int beforeRepaint = decoded;
        window.update();
        QTRY_VERIFY_WITH_TIMEOUT(decoded > beforeRepaint, 5000);
        QVERIFY2(deviceError.isEmpty(), qPrintable(deviceError));
        // A window resize keeps the fixed profile canvas and the new aspect
        // ratio: 720x300 is wider than 16:9, so the top/bottom become black.
        animation.start(33);
        const int beforeResize = decoded;
        window.resize(720, 300);
        // Frames already queued before the resize still arrive afterwards.
        // Wait for the actual geometry within the same six-second limit,
        // rather than treating three more frames as a resize acknowledgement.
        QElapsedTimer resizeWait;
        resizeWait.start();
        while (resizeWait.elapsed() < 6000 &&
               !(decoded > beforeResize + 2 && last.pixelColor(640, 3).lightness() < 30 &&
                 last.pixelColor(640, 717).lightness() < 30 &&
                 last.pixelColor(640, 360).lightness() > 40))
            QTest::qWait(20);
        QVERIFY(decoded > beforeResize + 2);
        QCOMPARE(last.size(), QSize(1280, 720));
        QVERIFY2(last.pixelColor(640, 3).lightness() < 30, "top edge must be a black bar");
        QVERIFY2(last.pixelColor(640, 717).lightness() < 30, "bottom edge must be a black bar");
        QVERIFY2(last.pixelColor(640, 360).lightness() > 40, "center must keep the window content");
        animation.stop();
        QVERIFY2(deviceError.isEmpty(), qPrintable(deviceError));
        QVERIFY(close(*s));
        QTRY_VERIFY(broker->idle());
    }

  private:
    void exerciseClosedWindowCapture(int cycles) {
        if (!qEnvironmentVariableIntValue("ZTO_TEST_PHYSICAL_VIDEO"))
            QSKIP("Set ZTO_TEST_PHYSICAL_VIDEO=1 for device capture");
        for (int cycle = 0; cycle < cycles; ++cycle) {
        QPointer<CaptureFixtureWindow> window = new CaptureFixtureWindow;
        const auto windowCleanup = qScopeGuard([&] { if (window) delete window.data(); });
        window->setWindowTitle(QStringLiteral("ZTOrbit close fixture ") + QUuid::createUuid().toString());
        window->resize(480, 360);
        window->show();
        QTest::qWait(100);
        window->hide();
        window->show();
        QTimer animation;
        QObject::connect(&animation, &QTimer::timeout, window.data(), [window] { if (window) window->update(); });
        animation.start(33);
        QTest::qWait(400);
        auto s = consumer();
        auto sources = await<media::SourceSnapshot>([&](auto *ctx, auto cb) { s->sources(ctx, cb); });
        QVERIFY(sources.ok());
        QString source;
        for (const auto &d : sources.value.sources)
            if (d.kind == "window" && d.description.contains(window->windowTitle())) {
                source = d.kind + ":" + d.id;
                break;
            }
        if (source.isEmpty()) {
            delete window.data();
            QVERIFY(close(*s));
            QSKIP("Fixture window is not capturable on this platform");
        }
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        auto tx = create(*s, video(source));
        QVERIFY2(tx.ok(), qPrintable(tx.error.message));
        auto c = video();
        c.direction = "receive";
        c.source.clear();
        auto rx = create(*s, c);
        QVERIFY(rx.ok());
        int decoded = 0;
        QString failure;
        const auto callbacks = qScopeGuard([&] {
            s->packet = {}; s->frame = {}; s->error = {};
            if (!broker->idle()) close(*s);
        });
        s->error = [&](const media::MediaError &e) {
            if (!failure.isEmpty())
                return;
            if (e.code == QStringLiteral("device") || e.code == QStringLiteral("track"))
                failure = e.message;
        };
        s->packet = [&](media::EncodedPacket p) { QVERIFY(s->submitTo(rx.value, std::move(p))); };
        s->frame = [&](const media::FrameView &f) {
            if (f.decoded)
                ++decoded;
        };
        QVERIFY(start(*s, rx.value.id));
        QVERIFY(start(*s, tx.value.id));
        QTRY_VERIFY_WITH_TIMEOUT(decoded >= 5, 10000);
        animation.stop();
        window->close();
        window->deleteLater();
        QTest::qWait(200);
        // The validity probe waits for a sustained absence, not one missed
        // enumeration, and then reports an explicit source failure.
        QTRY_VERIFY_WITH_TIMEOUT(!failure.isEmpty(), 20000);
        QVERIFY(close(*s));
        QTRY_VERIFY(broker->idle());
        qInfo() << "closed window cycle" << cycle;
        }
    }

  private slots:
    void physicalClosedWindowFailsCapture() { exerciseClosedWindowCapture(1); }
    void physicalClosedWindowsRepeatedly() { exerciseClosedWindowCapture(20); }

    void physicalStaticWindowSharedConsumerAndStop() {
        if (!qEnvironmentVariableIntValue("ZTO_TEST_PHYSICAL_VIDEO"))
            QSKIP("Set ZTO_TEST_PHYSICAL_VIDEO=1 for real window capture");
        CaptureFixtureWindow window;
        window.setWindowTitle(QStringLiteral("ZTOrbit static shared source ") + QUuid::createUuid().toString());
        window.resize(320,240);
        window.show();
        QTest::qWait(100); window.hide(); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(window.windowHandle()));
        QTest::qWait(300);
        auto s = consumer();
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        auto sources = await<media::SourceSnapshot>([&](auto *ctx, auto cb) { s->sources(ctx, cb); });
        QVERIFY(sources.ok());
        QString source;
        for (const auto &entry : sources.value.sources)
            if (entry.kind == "window" && entry.description == window.windowTitle()) source = "window:" + entry.id;
        QVERIFY(!source.isEmpty());
        auto first = create(*s, video(source)), second = create(*s, video(source));
        QVERIFY(first.ok()); QVERIFY(second.ok());
        int firstPackets = 0, secondPackets = 0;
        qint64 secondFirstPts = -1;
        const auto callbacks = qScopeGuard([&] {
            s->packet = {}; s->frame = {}; s->error = {};
            if (!broker->idle()) close(*s);
        });
        s->packet = [&](media::EncodedPacket packet) {
            if (packet.track == first.value.id) ++firstPackets;
            if (packet.track == second.value.id) {
                if (!secondPackets) secondFirstPts = packet.pts;
                ++secondPackets;
            }
        };
        QVERIFY(start(*s, first.value.id));
        QTRY_VERIFY_WITH_TIMEOUT(firstPackets > 0, 5000);
        QTest::qWait(1000); // A cached seed must not anchor a new stream a second in the past.
        const auto secondStartedAt = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        QVERIFY(start(*s, second.value.id));
        QTRY_VERIFY_WITH_TIMEOUT(secondPackets > 0, 5000);
        QVERIFY2(secondFirstPts >= secondStartedAt,
                 qPrintable(QStringLiteral("cached seed PTS %1 precedes attach %2").arg(secondFirstPts).arg(secondStartedAt)));
        auto stats = await<media::Statistics>([&](auto *ctx, auto cb) { s->statistics(ctx, cb); });
        QVERIFY(stats.ok());
        const auto secondTrack = stats.value.track(second.value.id);
        QVERIFY(secondTrack["videoBeforeStart"].toString().toULongLong() > 0);
        QVERIFY(secondTrack["captured"].toString().toULongLong() > 0);
        QCOMPARE(stats.value.values["devices"].toObject()["physicalInputs"].toInt(),1);
#ifdef Q_OS_WIN
        const auto native = stats.value.values["devices"].toObject()["sources"].toArray().first().toObject()["native"].toObject();
        QCOMPARE(native["apartmentType"].toInt(-1), 1); // APTTYPE_MTA, never silently accept an existing STA.
#endif
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->stop(first.value.id,ctx,cb); }).ok());
        const auto stoppedFirst = firstPackets, previousSecond = secondPackets;
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->requestKeyFrame(second.value.id,ctx,cb); }).ok());
        QTRY_VERIFY_WITH_TIMEOUT(secondPackets > previousSecond, 500);
        QCOMPARE(firstPackets,stoppedFirst);
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->stop(second.value.id,ctx,cb); }).ok());
        auto stopped = await<media::Statistics>([&](auto *ctx, auto cb) { s->statistics(ctx, cb); });
        QVERIFY(stopped.ok());
        QCOMPARE(stopped.value.values["devices"].toObject()["physicalInputs"].toInt(),0);
        const auto raw = stopped.value.values["devices"].toObject()["rawVideoFrames"];
        QTest::qWait(800);
        auto drained = await<media::Statistics>([&](auto *ctx, auto cb) { s->statistics(ctx, cb); });
        QVERIFY(drained.ok());
        QCOMPARE(drained.value.values["devices"].toObject()["rawVideoFrames"],raw);
        const auto beforeRestart = secondPackets;
        QVERIFY(start(*s, second.value.id));
        QTRY_VERIFY_WITH_TIMEOUT(secondPackets > beforeRestart,5000);
        qInfo().noquote() << "shared static startup" << QJsonDocument(stats.value.values).toJson(QJsonDocument::Compact);
        QVERIFY(close(*s));
        QTRY_VERIFY(broker->idle());
    }

    void physicalStaticScreenDoesNotFail() {
        if (!qEnvironmentVariableIntValue("ZTO_TEST_PHYSICAL_VIDEO"))
            QSKIP("Set ZTO_TEST_PHYSICAL_VIDEO=1 for device capture");
        CaptureFixtureWindow window;
        window.setWindowTitle("ZTOrbit static fixture");
        window.setWindowFlag(Qt::WindowStaysOnTopHint);
        window.resize(400, 300);
        window.move(80, 80);
        window.show();
        // The unattended Windows launcher hides the process's first show.
        // Exercise a second native show, as the other physical fixtures do,
        // before checking exposure or starting screen capture.
        QTest::qWait(100);
        window.hide();
        window.show();
        window.raise();
        QVERIFY(QTest::qWaitForWindowExposed(window.windowHandle()));
        QTimer animation;
        QObject::connect(&animation, &QTimer::timeout, &window, [&window] { window.update(); });
        animation.start(33);
        QTest::qWait(400);
        const auto screenName = window.screen() ? window.screen()->name() : QString{};
        auto s = consumer();
        auto sources = await<media::SourceSnapshot>([&](auto *ctx, auto cb) { s->sources(ctx, cb); });
        QVERIFY(sources.ok());
        QString source;
        for (const auto &d : sources.value.sources)
            if (d.kind == "screen" && (screenName.isEmpty() || d.id == screenName)) {
                source = d.kind + ":" + d.id;
                break;
            }
        if (source.isEmpty()) {
            QVERIFY(close(*s));
            QSKIP("Requested physical screen unavailable");
        }
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        auto tx = create(*s, video(source));
        QVERIFY2(tx.ok(), qPrintable(tx.error.message));
        auto c = video();
        c.direction = "receive";
        c.source.clear();
        auto rx = create(*s, c);
        QVERIFY(rx.ok());
        int decoded = 0, packets = 0;
        QString failure;
        const auto callbacks=qScopeGuard([&] {
            s->packet={};s->frame={};s->error={};
            if (!broker->idle()) {
                const auto stats = await<media::Statistics>([&](auto *ctx, auto cb) { s->statistics(ctx, cb); });
                if (stats.ok()) qInfo().noquote() << "static screen final" << "packets" << packets
                    << "decoded" << decoded << "failure" << failure
                    << QJsonDocument(stats.value.values).toJson(QJsonDocument::Compact);
                close(*s);
            }
        });
        s->error = [&](const media::MediaError &e) {
            if (e.code == QStringLiteral("device") || e.code == QStringLiteral("track"))
                failure = e.message;
        };
        s->packet = [&](media::EncodedPacket p) { ++packets;QVERIFY(s->submitTo(rx.value, std::move(p))); };
        s->frame = [&](const media::FrameView &f) {
            if (f.decoded)
                ++decoded;
        };
        QVERIFY(start(*s, rx.value.id));
        QVERIFY(start(*s, tx.value.id));
        QTRY_VERIFY_WITH_TIMEOUT(decoded >= 10, 15000);
        // Long static picture: no capture error, no automatic room/capture end.
        animation.stop();
        QTest::qWait(11000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        const int beforeRepaint = decoded;
        window.raise();
        window.update();
        QElapsedTimer recovery;recovery.start();
        while(decoded<=beforeRepaint && recovery.elapsed()<5000)QTest::qWait(20);
        const bool recovered=decoded>beforeRepaint;
        const auto stats=await<media::Statistics>([&](auto*ctx,auto cb){s->statistics(ctx,cb);});
        if(stats.ok())qInfo().noquote()<<"static screen recovery"<<"packets"<<packets
            <<"decoded"<<decoded<<QJsonDocument(stats.value.values).toJson(QJsonDocument::Compact);
        QVERIFY(recovered);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY(close(*s));
        QTRY_VERIFY(broker->idle());
    }

    void physicalCapture_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("screen") << QString("screen");
        QTest::newRow("window") << QString("window");
        QTest::newRow("camera") << QString("camera");
    }
    void physicalCapture() {
        if (!qEnvironmentVariableIntValue("ZTO_TEST_PHYSICAL_VIDEO"))
            QSKIP("Set ZTO_TEST_PHYSICAL_VIDEO=1 for device capture");
        QFETCH(QString, kind);
        CaptureFixtureWindow window;
        QTimer animation;
        connect(&animation, &QTimer::timeout, &window, [&] { window.update(); });
        window.setWindowTitle("ZTOrbit media capture fixture");
        window.resize(640, 360);
        if (kind == "window") {
            window.show();
            QTest::qWait(100);
            window.hide();
            window.show();
            animation.start(33);
            QTest::qWait(300);
        }
        auto s = consumer();
        auto sources = await<media::SourceSnapshot>([&](auto *ctx, auto cb) { s->sources(ctx, cb); });
        QVERIFY(sources.ok());
        QString source;
        for (const auto &d : sources.value.sources)
            if (d.kind == kind && (kind != "window" || d.description.contains(window.windowTitle()))) {
                source = d.kind + ":" + d.id;
                break;
            }
        if (source.isEmpty()) {
            QVERIFY(close(*s));
            QSKIP("Requested physical source unavailable");
        }
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        auto tx = create(*s, video(source));
        QVERIFY2(tx.ok(), qPrintable(tx.error.message));
        auto c = video();
        c.direction = "receive";
        c.source.clear();
        auto rx = create(*s, c);
        QVERIFY(rx.ok());
        int decoded = 0;
        s->packet = [&](media::EncodedPacket p) {
            p.track = rx.value.id;
            s->submit(std::move(p));
        };
        s->frame = [&](const media::FrameView &f) {
            if (f.decoded)
                ++decoded;
        };
        s->error = [](const media::MediaError &e) { qWarning() << e.message; };
        QVERIFY(start(*s, rx.value.id));
        QVERIFY(start(*s, tx.value.id));
        QElapsedTimer captureWait;
        captureWait.start();
        while (decoded < 10 && captureWait.elapsed() < 15000)
            QTest::qWait(20);
        auto observed = await<media::Statistics>([&](auto *ctx, auto cb) { s->statistics(ctx, cb); });
        qInfo() << "Capture observation:" << kind << decoded << observed.value.values;
        const bool enough = decoded >= 10;
        QVERIFY(close(*s));
        QVERIFY(enough);
        qInfo() << "Physical H.264 loopback verified:" << kind << decoded;
    }
    void physicalAudioLoopback() {
        if (!qEnvironmentVariableIntValue("ZTO_TEST_PHYSICAL_AUDIO"))
            QSKIP("Set ZTO_TEST_PHYSICAL_AUDIO=1 for microphone capture");
        auto s = consumer();
        auto sources = await<media::SourceSnapshot>([&](auto *ctx, auto cb) { s->sources(ctx, cb); });
        QVERIFY(sources.ok());
        QString input, output;
        for (const auto &d : sources.value.sources) {
            if (d.kind == "microphone" && (input.isEmpty() || d.isDefault))
                input = d.id;
            if (d.kind == "speaker" && (output.isEmpty() || d.isDefault))
                output = d.id;
        }
        if (input.isEmpty() || output.isEmpty()) {
            QVERIFY(close(*s));
            QSKIP("Microphone or speaker unavailable");
        }
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        media::TrackConfig c;
        c.source = input;
        auto tx = create(*s, c);
        QVERIFY(tx.ok());
        c.direction = "receive";
        c.output = output;
        c.gain = 0;
        auto rx = create(*s, c);
        QVERIFY(rx.ok());
        int packets = 0;
        s->packet = [&](media::EncodedPacket p) {
            ++packets;
            p.track = rx.value.id;
            s->submit(std::move(p));
        };
        QVERIFY(start(*s, rx.value.id));
        QVERIFY(start(*s, tx.value.id));
        QTRY_VERIFY_WITH_TIMEOUT(packets >= 20, 10000);
        auto stats = await<media::Statistics>([&](auto *ctx, auto cb) { s->statistics(ctx, cb); });
        QVERIFY(stats.ok());
        qInfo() << "Physical audio counters:" << stats.value.values;
        QVERIFY(close(*s));
    }
    void sustainedVideo() {
        int seconds = qEnvironmentVariableIntValue("ZTO_MEDIA_SOAK_SECONDS");
        if (seconds < 1)
            QSKIP("Set ZTO_MEDIA_SOAK_SECONDS=600 for performance acceptance");
        auto s = consumer();
        QVERIFY(await<bool>([&](auto *ctx, auto cb) { s->open(ctx, cb); }).ok());
        auto tx = create(*s, video());
        QVERIFY(tx.ok());
        auto c = video();
        c.direction = "receive";
        c.source.clear();
        auto rx = create(*s, c);
        QVERIFY(rx.ok());
        quint64 packets = 0, frames = 0;
        s->packet = [&](media::EncodedPacket p) {
            ++packets;
            p.track = rx.value.id;
            s->submit(std::move(p));
        };
        s->frame = [&](const media::FrameView &f) {
            if (f.decoded)
                ++frames;
        };
        QVERIFY(start(*s, rx.value.id));
        QVERIFY(start(*s, tx.value.id));
        QFile file(QCoreApplication::applicationDirPath() + "/media-soak.csv");
        QVERIFY(file.open(QIODevice::WriteOnly));
        QTextStream csv(&file);
        csv << "seconds,packets,decoded,workerPrivateBytes\n";
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < qint64(seconds) * 1000) {
            QTest::qWait(1000);
            quint64 memory = 0;
#ifdef Q_OS_WIN
            HANDLE process =
                OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, DWORD(broker->pid()));
            if (process) {
                PROCESS_MEMORY_COUNTERS_EX m{};
                m.cb = sizeof m;
                if (K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m),
                                            sizeof m))
                    memory = m.PrivateUsage;
                CloseHandle(process);
            }
#endif
            csv << timer.elapsed() / 1000 << ',' << packets << ',' << frames << ',' << memory << '\n';
            csv.flush();
        }
        QVERIFY(frames > quint64(seconds) * 10);
        auto stats = await<media::Statistics>([&](auto *ctx, auto cb) { s->statistics(ctx, cb); });
        QVERIFY(stats.ok());
        qInfo() << "Final sustained video stats:" << stats.value.values;
        QVERIFY(close(*s));
    }
};
QTEST_MAIN(MediaIntegrationTests)
#include "MediaIntegrationTests.moc"
