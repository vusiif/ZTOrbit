#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtEndian>
#include <algorithm>

namespace vusiif::ztgui::media::engine {
// Strict RIFF/WAVE PCM16 48 kHz mono parser for short clips. Header-declared
// lengths are never trusted without checking the actual payload and padding.
struct ClipAudio {
    QVector<float> samples;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

inline quint32 clipLe32(const QByteArray &data, qsizetype offset)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data.constData() + offset));
}

inline ClipAudio parseClipWav(const QByteArray &wav)
{
    ClipAudio result;
    if (wav.isEmpty()) { result.error = QStringLiteral("WAV payload is empty"); return result; }
    if (wav.size() > 256 * 1024) { result.error = QStringLiteral("WAV payload exceeds 256 KiB"); return result; }
    if (wav.size() < 12 || wav.constData()[0] != 'R' || wav.constData()[1] != 'I' ||
        wav.constData()[2] != 'F' || wav.constData()[3] != 'F' ||
        wav.constData()[8] != 'W' || wav.constData()[9] != 'A' ||
        wav.constData()[10] != 'V' || wav.constData()[11] != 'E') {
        result.error = QStringLiteral("Not a RIFF/WAVE payload");
        return result;
    }
    const quint32 declared = clipLe32(wav, 4);
    if (declared < 4 || qint64(declared) + 8 > wav.size()) {
        result.error = QStringLiteral("RIFF length exceeds the payload");
        return result;
    }
    bool haveFormat = false, haveData = false;
    qsizetype dataOffset = 0, dataSize = 0;
    qsizetype offset = 12;
    while (offset + 8 <= wav.size()) {
        const auto id = wav.mid(offset, 4);
        const quint32 size = clipLe32(wav, offset + 4);
        const qsizetype body = offset + 8;
        if (qint64(size) > wav.size() - body) {
            result.error = QStringLiteral("Chunk length exceeds the payload");
            return result;
        }
        if (id == "fmt ") {
            if (haveFormat) { result.error = QStringLiteral("Duplicate fmt chunk"); return result; }
            haveFormat = true;
            if (size < 16) { result.error = QStringLiteral("fmt chunk is too short"); return result; }
            const quint16 format = qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(wav.constData() + body));
            const quint16 channels = qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(wav.constData() + body + 2));
            const quint32 sampleRate = clipLe32(wav, body + 4);
            const quint32 byteRate = clipLe32(wav, body + 8);
            const quint16 blockAlign = qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(wav.constData() + body + 12));
            const quint16 bits = qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(wav.constData() + body + 14));
            if (format != 1 || channels != 1 || sampleRate != 48000 || bits != 16 ||
                blockAlign != 2 || byteRate != 96000) {
                result.error = QStringLiteral("Only PCM16 48 kHz mono WAV is supported");
                return result;
            }
        } else if (id == "data") {
            if (haveData) { result.error = QStringLiteral("Duplicate data chunk"); return result; }
            haveData = true;
            dataOffset = body;
            dataSize = size;
        }
        offset = body + size + (size & 1); // RIFF chunks are word aligned.
        if (offset > wav.size()) { result.error = QStringLiteral("Chunk padding exceeds the payload"); return result; }
    }
    if (!haveFormat) { result.error = QStringLiteral("Missing fmt chunk"); return result; }
    if (!haveData) { result.error = QStringLiteral("Missing data chunk"); return result; }
    if (dataSize < 2 || (dataSize & 1)) { result.error = QStringLiteral("data chunk is not aligned PCM16"); return result; }
    const auto frameCount = dataSize / 2;
    if (frameCount > 48000 * 2) { result.error = QStringLiteral("Clip exceeds 2 seconds"); return result; }
    result.samples.resize(frameCount);
    const auto *dataBytes = reinterpret_cast<const uchar *>(wav.constData() + dataOffset);
    for (qsizetype index = 0; index < frameCount; ++index) {
        const qint16 value = qint16(qFromLittleEndian<quint16>(dataBytes + index * 2));
        result.samples[index] = float(value) / 32768.0f;
    }
    return result;
}
} // namespace vusiif::ztgui::media::engine
