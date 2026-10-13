#include "../../plugins/voice-chat/client/AudioMath.h"
#include <QtTest>
#include <cmath>
using namespace vusiif::ztgui::voice_chat;
class AudioTests : public QObject {
    Q_OBJECT
private slots:
    void packetValidation() {
        Packet input{Audio, QByteArray(16, 'x'), 42, 0xffffffff, "encoded"}, output;
        QVERIFY(decodePacket(encodePacket(input), output));
        QCOMPARE(output.sender, input.sender); QCOMPARE(output.sequence, input.sequence); QCOMPARE(output.payload, input.payload);
        QVERIFY(!decodePacket(QByteArray(1201, 'x'), output));
        auto bytes = encodePacket(input); bytes[4] = 2; QVERIFY(!decodePacket(bytes, output));
        QVERIFY(encodePacket({Audio, "short", 1, 1, "x"}).isEmpty());
    }
    void pcmConversion() {
        for (auto type : {QAudioFormat::UInt8, QAudioFormat::Int16, QAudioFormat::Int32, QAudioFormat::Float}) {
            QAudioFormat format; format.setSampleRate(48000); format.setChannelCount(2); format.setSampleFormat(type);
            const QVector<float> samples{-1, -0.5, 0, 0.5, 1};
            auto bytes = encodePcm(samples, format, 1); bytes += 'x';
            const auto decoded = decodePcm(bytes, format);
            QCOMPARE(decoded.size(), samples.size()); QCOMPARE(bytes.size(), qsizetype(1));
            for (int i = 0; i < samples.size(); ++i) QVERIFY(std::abs(decoded[i] - samples[i]) < 0.02f);
        }
        QCOMPARE(clampSample(4), 1.0f); QCOMPARE(clampSample(NAN), 0.0f);
    }
    void streamingResample() {
        Resampler resampler;
        QVector<float> result;
        for (int i = 0; i < 10; ++i) result += resampler.process(QVector<float>(441, 0.25f), 44100, 48000);
        QVERIFY(std::abs(result.size() - 4800) <= 2);
        for (float sample : result) QVERIFY(std::abs(sample - 0.25f) < 0.001f);
    }
    void codecAndJitter() {
        int error = 0; auto *encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error); QVERIFY(encoder);
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
        JitterStream stream;
        QVector<QByteArray> encoded;
        for (int frame = 0; frame < 8; ++frame) {
            QVector<float> pcm(FrameSamples);
            for (int i = 0; i < FrameSamples; ++i) pcm[i] = 0.25f * std::sin((frame * FrameSamples + i) * 2 * 3.141592653589793 * 440 / 48000);
            QByteArray data(1170, '\0'); const int size = opus_encode_float(encoder, pcm.constData(), FrameSamples, reinterpret_cast<unsigned char *>(data.data()), int(data.size()));
            QVERIFY(size > 0); data.resize(size); encoded.append(data);
        }
        // Reordering, duplicate and a missing packet, across sequence wrap.
        const quint32 start = 0xfffffffd;
        stream.push(start + 1, encoded[1]); stream.push(start, encoded[0]); stream.push(start, encoded[0]);
        for (int i = 2; i < 8; ++i) if (i != 4) stream.push(start + i, encoded[i]);
        float energy = 0;
        for (int i = 0; i < 11; ++i) for (float sample : stream.pull()) { QVERIFY(std::isfinite(sample)); energy += sample * sample; }
        QVERIFY(energy > 1);
        for (int i = 0; i < 100; ++i) stream.push(i + 100, encoded[0]);
        QCOMPARE(stream.pull().size(), qsizetype(FrameSamples));
        opus_encoder_destroy(encoder);
    }
};
QTEST_GUILESS_MAIN(AudioTests)
#include "AudioTests.moc"
