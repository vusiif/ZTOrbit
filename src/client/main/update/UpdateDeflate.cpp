#include "UpdateDeflate.h"

#include <QByteArray>

#include <algorithm>
#include <array>
#include <vector>

namespace vusiif::zto::update {
namespace {

constexpr int MaxBits = 15;
constexpr int MaxLitLengthCodes = 286;
constexpr int MaxDistanceCodes = 30;

struct Huffman {
    std::array<int, MaxBits + 1> count{};
    std::vector<int> symbols;
};

class BitReader {
public:
    explicit BitReader(InflateSource &source) : m_source(source) {}

    bool ensure(int bits)
    {
        while (m_bitCount < bits) {
            if (m_bytePosition >= m_bytes.size()) {
                m_bytes.resize(64 * 1024);
                m_bytePosition = 0;
                const qint64 read = m_source.read(reinterpret_cast<unsigned char *>(m_bytes.data()),
                                                  static_cast<qint64>(m_bytes.size()));
                if (read < 0) {
                    m_error = QStringLiteral("读取压缩数据失败");
                    return false;
                }
                m_bytes.resize(read);
                if (read == 0) {
                    m_error = QStringLiteral("压缩数据意外结束");
                    return false;
                }
            }
            m_bitBuffer |= static_cast<std::uint64_t>(static_cast<unsigned char>(m_bytes.at(m_bytePosition)))
                           << m_bitCount;
            ++m_bytePosition;
            m_bitCount += 8;
        }
        return true;
    }

    int bits(int count)
    {
        if (count == 0) return 0;
        if (!ensure(count)) return -1;
        const int value = static_cast<int>(m_bitBuffer & ((static_cast<std::uint64_t>(1) << count) - 1));
        m_bitBuffer >>= count;
        m_bitCount -= count;
        return value;
    }

    void alignToByte()
    {
        const int drop = m_bitCount % 8;
        m_bitBuffer >>= drop;
        m_bitCount -= drop;
    }

    bool readBytes(unsigned char *destination, qint64 count)
    {
        // Caller must have aligned the reader to a byte boundary first.
        while (count > 0) {
            if (m_bitCount % 8 != 0) return fail(QStringLiteral("压缩数据未按字节对齐"));
            if (m_bitCount > 0) {
                if (m_bitCount < 8) return fail(QStringLiteral("压缩数据位缓冲异常"));
                *destination = static_cast<unsigned char>(m_bitBuffer & 0xff);
                m_bitBuffer >>= 8;
                m_bitCount -= 8;
                ++destination;
                --count;
                continue;
            }
            if (m_bytePosition >= m_bytes.size()) {
                if (!ensure(8)) return false;
                continue;
            }
            const qint64 available = static_cast<qint64>(m_bytes.size()) - m_bytePosition;
            if (available <= 0) {
                if (!ensure(8)) return false;
                continue;
            }
            const qint64 chunk = std::min(available, count);
            std::copy_n(reinterpret_cast<const unsigned char *>(m_bytes.constData()) + m_bytePosition,
                        static_cast<std::size_t>(chunk), destination);
            m_bytePosition += chunk;
            destination += chunk;
            count -= chunk;
        }
        return true;
    }

    const QString &error() const { return m_error; }
    bool fail(const QString &message)
    {
        if (m_error.isEmpty()) m_error = message;
        return false;
    }

private:
    InflateSource &m_source;
    QByteArray m_bytes;
    qint64 m_bytePosition = 0;
    std::uint64_t m_bitBuffer = 0;
    int m_bitCount = 0;
    QString m_error;
};

bool buildHuffman(const std::vector<int> &lengths, Huffman &out, QString &error)
{
    out.count.fill(0);
    for (int length : lengths) {
        if (length < 0 || length > MaxBits) {
            error = QStringLiteral("无效的霍夫曼码长");
            return false;
        }
        if (length > 0) ++out.count[length];
    }
    int left = 1;
    for (int length = 1; length <= MaxBits; ++length) {
        left <<= 1;
        left -= out.count[length];
        if (left < 0) {
            error = QStringLiteral("霍夫曼表超额订阅");
            return false;
        }
    }
    int total = 0;
    for (int length = 1; length <= MaxBits; ++length) total += out.count[length];
    if (total == 0) {
        error = QStringLiteral("空霍夫曼表");
        return false;
    }
    // An incomplete tree is only valid for the degenerate single-symbol case.
    if (left > 0 && total != 1) {
        error = QStringLiteral("霍夫曼表不完整");
        return false;
    }
    std::array<int, MaxBits + 1> offsets{};
    int sum = 0;
    for (int length = 1; length <= MaxBits; ++length) {
        offsets[length] = sum;
        sum += out.count[length];
    }
    out.symbols.assign(static_cast<std::size_t>(total), 0);
    for (int symbol = 0; symbol < static_cast<int>(lengths.size()); ++symbol) {
        const int length = lengths[symbol];
        if (length == 0) continue;
        out.symbols[static_cast<std::size_t>(offsets[length]++)] = symbol;
    }
    return true;
}

int decodeSymbol(BitReader &reader, const Huffman &table)
{
    int code = 0;
    int first = 0;
    int index = 0;
    for (int length = 1; length <= MaxBits; ++length) {
        const int bit = reader.bits(1);
        if (bit < 0) return -1;
        code |= bit;
        const int count = table.count[length];
        if (code - first < count) return table.symbols[static_cast<std::size_t>(index + (code - first))];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

class Output {
public:
    Output(InflateSink &sink, qint64 maximum) : m_sink(sink), m_maximum(maximum) {}
    bool put(unsigned char value)
    {
        if (m_total >= m_maximum) return false;
        m_window[m_position] = value;
        m_position = (m_position + 1) % WindowSize;
        ++m_total;
        ++m_pending;
        if (m_pending == WindowSize) return flush();
        return true;
    }
    bool copyDistance(int distance, int length)
    {
        if (distance <= 0 || distance > static_cast<int>(std::min<quint64>(m_total, WindowSize)))
            return false;
        for (int index = 0; index < length; ++index) {
            const unsigned char value = m_window[(m_position + WindowSize - distance) % WindowSize];
            if (!put(value)) return false;
        }
        return true;
    }
    bool flush()
    {
        if (m_pending == 0) return true;
        const std::size_t start = (m_position + WindowSize - m_pending) % WindowSize;
        const std::size_t first = std::min<std::size_t>(m_pending, WindowSize - start);
        if (first > 0
            && !m_sink.write(m_window.data() + start, static_cast<qint64>(first)))
            return false;
        const std::size_t second = m_pending - first;
        if (second > 0 && !m_sink.write(m_window.data(), static_cast<qint64>(second))) return false;
        m_pending = 0;
        return true;
    }
    quint64 total() const { return m_total; }

private:
    static constexpr std::size_t WindowSize = 32768;
    InflateSink &m_sink;
    qint64 m_maximum;
    std::array<unsigned char, WindowSize> m_window{};
    std::size_t m_position = 0;
    std::size_t m_pending = 0;
    quint64 m_total = 0;
};

const std::array<int, 29> LengthBase = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const std::array<int, 29> LengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                         2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const std::array<int, 30> DistanceBase = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                           193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                           6145, 8193, 12289, 16385, 24577};
const std::array<int, 30> DistanceExtra = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                           6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const std::array<int, 19> CodeLengthOrder = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

bool inflateBlockData(BitReader &reader, Output &output, const Huffman &literalTable,
                      const Huffman &distanceTable)
{
    for (;;) {
        const int symbol = decodeSymbol(reader, literalTable);
        if (symbol < 0) return reader.fail(QStringLiteral("无效的字面量/长度符号"));
        if (symbol < 256) {
            if (!output.put(static_cast<unsigned char>(symbol))) {
                return reader.fail(QStringLiteral("输出超过大小限制"));
            }
            continue;
        }
        if (symbol == 256) return true; // end of block
        const int lengthIndex = symbol - 257;
        if (lengthIndex < 0 || lengthIndex >= static_cast<int>(LengthBase.size()))
            return reader.fail(QStringLiteral("无效的长度符号"));
        const int lengthExtra = LengthExtra[static_cast<std::size_t>(lengthIndex)];
        const int extraLength = lengthExtra == 0 ? 0 : reader.bits(lengthExtra);
        if (extraLength < 0) return false;
        const int length = LengthBase[static_cast<std::size_t>(lengthIndex)] + extraLength;

        const int distanceSymbol = decodeSymbol(reader, distanceTable);
        if (distanceSymbol < 0 || distanceSymbol >= MaxDistanceCodes)
            return reader.fail(QStringLiteral("无效的距离符号"));
        const int distanceExtra = DistanceExtra[static_cast<std::size_t>(distanceSymbol)];
        const int extraDistance = distanceExtra == 0 ? 0 : reader.bits(distanceExtra);
        if (extraDistance < 0) return false;
        const int distance = DistanceBase[static_cast<std::size_t>(distanceSymbol)] + extraDistance;
        if (!output.copyDistance(distance, length))
            return reader.fail(QStringLiteral("无效的回溯距离或输出超过大小限制"));
    }
}

} // namespace

InflateResult inflateRaw(InflateSource &source, InflateSink &sink, qint64 maximumBytes,
                         qint64 expectedBytes)
{
    InflateResult result;
    if (maximumBytes <= 0) {
        result.error = QStringLiteral("解压大小限制无效");
        return result;
    }
    BitReader reader(source);
    Output output(sink, maximumBytes);

    std::vector<int> fixedLiteralLengths(288);
    for (int index = 0; index < 144; ++index) fixedLiteralLengths[static_cast<std::size_t>(index)] = 8;
    for (int index = 144; index < 256; ++index) fixedLiteralLengths[static_cast<std::size_t>(index)] = 9;
    for (int index = 256; index < 280; ++index) fixedLiteralLengths[static_cast<std::size_t>(index)] = 7;
    for (int index = 280; index < 288; ++index) fixedLiteralLengths[static_cast<std::size_t>(index)] = 8;
    std::vector<int> fixedDistanceLengths(32, 5);
    Huffman fixedLiteralTable;
    Huffman fixedDistanceTable;
    QString tableError;
    if (!buildHuffman(fixedLiteralLengths, fixedLiteralTable, tableError)
        || !buildHuffman(fixedDistanceLengths, fixedDistanceTable, tableError)) {
        result.error = tableError;
        return result;
    }

    bool final = false;
    while (!final) {
        const int finalBit = reader.bits(1);
        if (finalBit < 0) {
            result.error = reader.error();
            if (result.error.isEmpty()) result.error = QStringLiteral("无效的 DEFLATE 数据");
            return result;
        }
        final = finalBit == 1;
        const int type = reader.bits(2);
        if (type < 0) {
            result.error = reader.error();
            if (result.error.isEmpty()) result.error = QStringLiteral("无效的 DEFLATE 数据");
            return result;
        }
        if (type == 0) {
            reader.alignToByte();
            unsigned char header[4] = {};
            if (!reader.readBytes(header, 4)) {
                result.error = reader.error();
                return result;
            }
            const int length = header[0] | (header[1] << 8);
            const int complement = header[2] | (header[3] << 8);
            if ((length ^ 0xffff) != complement) {
                result.error = QStringLiteral("存储块长度校验失败");
                return result;
            }
            for (int index = 0; index < length; ++index) {
                unsigned char value = 0;
                if (!reader.readBytes(&value, 1)) {
                    result.error = reader.error();
                    return result;
                }
                if (!output.put(value)) {
                    result.error = QStringLiteral("输出超过大小限制");
                    return result;
                }
            }
            continue;
        }
        if (type == 3) {
            result.error = QStringLiteral("保留的 DEFLATE 块类型");
            return result;
        }
        Huffman literalTable;
        Huffman distanceTable;
        if (type == 1) {
            literalTable = fixedLiteralTable;
            distanceTable = fixedDistanceTable;
        } else {
            const int literalCount = reader.bits(5);
            const int distanceCount = reader.bits(5);
            const int codeLengthCount = reader.bits(4);
            if (literalCount < 0 || distanceCount < 0 || codeLengthCount < 0) {
                result.error = reader.error();
                return result;
            }
            const int literalTotal = literalCount + 257;
            const int distanceTotal = distanceCount + 1;
            const int codeLengthTotal = codeLengthCount + 4;
            if (literalTotal > MaxLitLengthCodes || distanceTotal > MaxDistanceCodes) {
                result.error = QStringLiteral("动态霍夫曼表超出规范上限");
                return result;
            }
            std::vector<int> codeLengthLengths(19, 0);
            for (int index = 0; index < codeLengthTotal; ++index) {
                const int value = reader.bits(3);
                if (value < 0) {
                    result.error = reader.error();
                    return result;
                }
                codeLengthLengths[static_cast<std::size_t>(CodeLengthOrder[static_cast<std::size_t>(index)])] = value;
            }
            Huffman codeLengthTable;
            if (!buildHuffman(codeLengthLengths, codeLengthTable, tableError)) {
                result.error = tableError;
                return result;
            }
            std::vector<int> lengths;
            lengths.reserve(static_cast<std::size_t>(literalTotal + distanceTotal));
            while (static_cast<int>(lengths.size()) < literalTotal + distanceTotal) {
                const int symbol = decodeSymbol(reader, codeLengthTable);
                if (symbol < 0) {
                    result.error = reader.error().isEmpty() ? QStringLiteral("无效的码长符号") : reader.error();
                    return result;
                }
                if (symbol < 16) {
                    lengths.push_back(symbol);
                } else if (symbol == 16) {
                    if (lengths.empty()) {
                        result.error = QStringLiteral("码长重复没有前值");
                        return result;
                    }
                    const int repeat = reader.bits(2);
                    if (repeat < 0) {
                        result.error = reader.error();
                        return result;
                    }
                    const int count = 3 + repeat;
                    const int previous = lengths.back();
                    lengths.insert(lengths.end(), static_cast<std::size_t>(count), previous);
                } else if (symbol == 17) {
                    const int repeat = reader.bits(3);
                    if (repeat < 0) {
                        result.error = reader.error();
                        return result;
                    }
                    lengths.insert(lengths.end(), static_cast<std::size_t>(3 + repeat), 0);
                } else if (symbol == 18) {
                    const int repeat = reader.bits(7);
                    if (repeat < 0) {
                        result.error = reader.error();
                        return result;
                    }
                    lengths.insert(lengths.end(), static_cast<std::size_t>(11 + repeat), 0);
                } else {
                    result.error = QStringLiteral("无效的码长符号");
                    return result;
                }
                if (static_cast<int>(lengths.size()) > literalTotal + distanceTotal) {
                    result.error = QStringLiteral("码长表超出声明的大小");
                    return result;
                }
            }
            std::vector<int> literalLengths(lengths.begin(), lengths.begin() + literalTotal);
            std::vector<int> distanceLengths(lengths.begin() + literalTotal, lengths.end());
            if (literalLengths[256] == 0) {
                result.error = QStringLiteral("缺少块结束符号");
                return result;
            }
            if (!buildHuffman(literalLengths, literalTable, tableError)
                || !buildHuffman(distanceLengths, distanceTable, tableError)) {
                result.error = tableError;
                return result;
            }
        }
        if (!inflateBlockData(reader, output, literalTable, distanceTable)) {
            result.error = reader.error();
            if (result.error.isEmpty()) result.error = QStringLiteral("无效的 DEFLATE 数据");
            return result;
        }
    }
    if (!output.flush()) {
        result.error = QStringLiteral("写入解压结果失败");
        return result;
    }
    result.bytesWritten = output.total();
    if (expectedBytes >= 0 && static_cast<qint64>(result.bytesWritten) != expectedBytes) {
        result.error = QStringLiteral("解压后大小与声明不一致");
        return result;
    }
    result.ok = true;
    return result;
}

namespace {

class BufferSource final : public InflateSource {
public:
    explicit BufferSource(const QByteArray &input) : m_input(input) {}
    qint64 read(unsigned char *destination, qint64 maximum) override
    {
        const qint64 available = static_cast<qint64>(m_input.size()) - m_position;
        const qint64 count = std::min(available, maximum);
        if (count <= 0) return 0;
        std::copy_n(reinterpret_cast<const unsigned char *>(m_input.constData()) + m_position,
                    static_cast<std::size_t>(count), destination);
        m_position += count;
        return count;
    }
private:
    const QByteArray &m_input;
    qint64 m_position = 0;
};

class BufferSink final : public InflateSink {
public:
    explicit BufferSink(QByteArray &output) : m_output(output) {}
    bool write(const unsigned char *data, qint64 size) override
    {
        m_output.append(reinterpret_cast<const char *>(data), static_cast<int>(size));
        return true;
    }
private:
    QByteArray &m_output;
};

} // namespace

InflateResult inflateRawBuffer(const QByteArray &input, QByteArray &output, qint64 maximumBytes)
{
    output.clear();
    BufferSource source(input);
    BufferSink sink(output);
    return inflateRaw(source, sink, maximumBytes, -1);
}

} // namespace vusiif::zto::update
