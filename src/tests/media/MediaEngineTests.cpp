#include "VideoCodec.h"
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
using namespace vusiif::ztgui;
namespace {
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
        const auto extension=qEnvironmentVariable("ZTGUI_MEDIA_EXTENSION_PATH",QString::fromUtf8(MEDIA_FLIP));
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
