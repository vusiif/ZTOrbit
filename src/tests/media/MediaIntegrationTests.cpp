#include "../../client/main/plugins/ServiceBroker.h"
#include "PluginPackage.h"
#include <ztmedia/MediaSession.h>
#include <QtTest>
#include <QTemporaryDir>
#include <QWindow>
#include <QFile>
#include <QWidget>
#include <QPainter>
#include <QtEndian>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif
using namespace vusiif::ztgui;
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
        qputenv("ZTGUI_MEDIA_TEST_CLIP_FAILURE", clipFailureFile.toUtf8());
    }
    void init() {
        QFile::remove(clipFailureFile);
        auto package = package::inspect(qEnvironmentVariable("ZTGUI_TEST_MEDIA_PACKAGE",QString::fromUtf8(MEDIA_PLUGIN)));
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
        QVERIFY(start(*a, receive.value.id));
        QVERIFY(start(*a, send.value.id));
        QVERIFY(start(*b, independent.value.id));
        QTRY_VERIFY_WITH_TIMEOUT(decoded >= 20 && other >= 20, 8000);
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
                             name != "qt6multimedia.dll" && name != "ztmedia_flip.dll",
                         qPrintable(name));
            } while (Module32NextW(snapshot, &module));
        CloseHandle(snapshot);
#endif
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
    void physicalCapture_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("screen") << QString("screen");
        QTest::newRow("window") << QString("window");
        QTest::newRow("camera") << QString("camera");
    }
    void physicalCapture() {
        if (!qEnvironmentVariableIntValue("ZTGUI_TEST_PHYSICAL_VIDEO"))
            QSKIP("Set ZTGUI_TEST_PHYSICAL_VIDEO=1 for device capture");
        QFETCH(QString, kind);
        CaptureFixtureWindow window;
        QTimer animation;
        connect(&animation, &QTimer::timeout, &window, [&] { window.update(); });
        window.setWindowTitle("ZTGUI media capture fixture");
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
        if (!qEnvironmentVariableIntValue("ZTGUI_TEST_PHYSICAL_AUDIO"))
            QSKIP("Set ZTGUI_TEST_PHYSICAL_AUDIO=1 for microphone capture");
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
        int seconds = qEnvironmentVariableIntValue("ZTGUI_MEDIA_SOAK_SECONDS");
        if (seconds < 1)
            QSKIP("Set ZTGUI_MEDIA_SOAK_SECONDS=600 for performance acceptance");
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
