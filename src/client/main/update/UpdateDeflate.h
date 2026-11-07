#pragma once

// Minimal raw DEFLATE (RFC 1951) decompressor used by the update package
// reader. Written in project code so the updater needs neither a private Qt ZIP
// API nor a vendored compression library. It is deliberately fail-closed: any
// malformed stream, over-subscribed Huffman table, invalid distance or output
// overrun ends Decompression with an error.

#include <QString>

#include <cstdint>
#include <functional>

namespace vusiif::zto::update {

class InflateSource {
public:
    virtual ~InflateSource() = default;
    // Reads up to `maximum` bytes; returns the number of bytes read (0 = end).
    virtual qint64 read(unsigned char *destination, qint64 maximum) = 0;
};

class InflateSink {
public:
    virtual ~InflateSink() = default;
    virtual bool write(const unsigned char *data, qint64 size) = 0;
};

struct InflateResult {
    bool ok = false;
    QString error;
    quint64 bytesWritten = 0;
};

// `maximumBytes` bounds the decompressed output. `expectedBytes`, when >= 0,
// must match the output size exactly.
InflateResult inflateRaw(InflateSource &source, InflateSink &sink, qint64 maximumBytes,
                         qint64 expectedBytes = -1);

// Convenience overload for tests and small buffers.
InflateResult inflateRawBuffer(const QByteArray &input, QByteArray &output, qint64 maximumBytes);

} // namespace vusiif::zto::update
