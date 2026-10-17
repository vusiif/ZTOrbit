#include "VideoCodec.h"
#include "AudioMath.h"
#include "MediaClock.h"
#include <ztmedia/SharedFrames.h>
#include <ztprotocol/BulkChannel.h>
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
using namespace vusiif::ztgui;
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
