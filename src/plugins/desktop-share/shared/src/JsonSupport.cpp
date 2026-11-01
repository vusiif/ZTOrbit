#include "JsonSupport.h"

#include <QStringConverter>
#include <QtGlobal>

#include <cctype>
#include <limits>
#include <utility>

namespace vusiif::zto::desktop_share::json {
namespace {

constexpr int MaxDepth = 64;

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool validNumber(QByteArrayView text)
{
    int i = 0;
    const int n = int(text.size());
    if (i < n && text[i] == '-') ++i;
    if (i >= n) return false;
    if (text[i] == '0') {
        ++i;
        if (i < n && isDigit(text[i])) return false;
    } else if (text[i] >= '1' && text[i] <= '9') {
        while (i < n && isDigit(text[i])) ++i;
    } else {
        return false;
    }
    if (i < n && text[i] == '.') {
        ++i;
        if (i >= n || !isDigit(text[i])) return false;
        while (i < n && isDigit(text[i])) ++i;
    }
    if (i < n && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < n && (text[i] == '+' || text[i] == '-')) ++i;
        if (i >= n || !isDigit(text[i])) return false;
        while (i < n && isDigit(text[i])) ++i;
    }
    return i == n;
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

class Parser {
public:
    Parser(QByteArrayView input, ParseError *error)
        : m_input(input), m_error(error)
    {
    }

    std::optional<Value> run()
    {
        // Validate the whole document as UTF-8 before treating syntax as ASCII:
        // JSON strings may contain any scalar value, but not arbitrary bytes.
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString decoded = decoder(m_input);
        Q_UNUSED(decoded);
        if (decoder.hasError()) {
            fail(QStringLiteral("输入不是合法的 UTF-8"), 0);
            return std::nullopt;
        }
        skipWhitespace();
        Value value;
        if (!parseValue(value, 0)) return std::nullopt;
        skipWhitespace();
        if (m_pos != m_input.size()) {
            fail(QStringLiteral("JSON 末尾存在多余内容"), m_pos);
            return std::nullopt;
        }
        return value;
    }

private:
    bool fail(const QString &message, int offset = -1)
    {
        if (m_error && m_error->message.isEmpty()) {
            m_error->offset = offset < 0 ? m_pos : offset;
            m_error->message = message;
        }
        return false;
    }

    void skipWhitespace()
    {
        while (m_pos < m_input.size()) {
            const char c = m_input[m_pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++m_pos;
            else break;
        }
    }

    bool consume(char expected)
    {
        if (m_pos >= m_input.size() || m_input[m_pos] != expected) {
            return fail(QStringLiteral("期望字符 '%1'").arg(QChar::fromLatin1(expected)));
        }
        ++m_pos;
        return true;
    }

    bool startsWith(QByteArrayView literal) const
    {
        if (m_input.size() - m_pos < literal.size()) return false;
        return m_input.mid(m_pos, literal.size()) == literal;
    }

    bool parseValue(Value &out, int depth)
    {
        if (depth > MaxDepth) return fail(QStringLiteral("JSON 嵌套过深"));
        if (m_pos >= m_input.size()) return fail(QStringLiteral("JSON 值缺失"));
        const char c = m_input[m_pos];
        if (c == '{') return parseObject(out, depth);
        if (c == '[') return parseArray(out, depth);
        if (c == '"') {
            QString text;
            if (!parseString(text)) return false;
            out = Value::string(std::move(text));
            return true;
        }
        if (c == 't') {
            if (!startsWith("true")) return fail(QStringLiteral("非法字面量"));
            m_pos += 4;
            out = Value(true);
            return true;
        }
        if (c == 'f') {
            if (!startsWith("false")) return fail(QStringLiteral("非法字面量"));
            m_pos += 5;
            out = Value(false);
            return true;
        }
        if (c == 'n') {
            if (!startsWith("null")) return fail(QStringLiteral("非法字面量"));
            m_pos += 4;
            out = Value::null();
            return true;
        }
        if (c == '-' || isDigit(c)) return parseNumber(out);
        return fail(QStringLiteral("非法 JSON 值"));
    }

    bool parseNumber(Value &out)
    {
        const int start = m_pos;
        while (m_pos < m_input.size()) {
            const char c = m_input[m_pos];
            if (c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E' || isDigit(c)) ++m_pos;
            else break;
        }
        const auto lexical = m_input.mid(start, m_pos - start);
        if (!validNumber(lexical)) {
            m_pos = start;
            return fail(QStringLiteral("非法 JSON 数字"));
        }
        out = Value::number(QString::fromLatin1(lexical));
        return true;
    }

    bool parseString(QString &out)
    {
        if (!consume('"')) return false;
        QByteArray pending;
        const auto flush = [&] {
            if (pending.isEmpty()) return;
            out += QString::fromUtf8(pending);
            pending.clear();
        };
        while (m_pos < m_input.size()) {
            const char c = m_input[m_pos++];
            if (c == '"') {
                flush();
                return true;
            }
            if (c == '\\') {
                flush();
                if (m_pos >= m_input.size()) return fail(QStringLiteral("字符串转义被截断"));
                const char escape = m_input[m_pos++];
                switch (escape) {
                case '"': out += QLatin1Char('"'); break;
                case '\\': out += QLatin1Char('\\'); break;
                case '/': out += QLatin1Char('/'); break;
                case 'b': out += QLatin1Char('\b'); break;
                case 'f': out += QLatin1Char('\f'); break;
                case 'n': out += QLatin1Char('\n'); break;
                case 'r': out += QLatin1Char('\r'); break;
                case 't': out += QLatin1Char('\t'); break;
                case 'u': {
                    if (m_pos + 4 > m_input.size())
                        return fail(QStringLiteral("\\u 转义被截断"));
                    int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const int digit = hexValue(m_input[m_pos + i]);
                        if (digit < 0) return fail(QStringLiteral("\\u 转义包含非十六进制字符"));
                        code = code * 16 + digit;
                    }
                    m_pos += 4;
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (m_pos + 6 > m_input.size() || m_input[m_pos] != '\\'
                            || m_input[m_pos + 1] != 'u')
                            return fail(QStringLiteral("高代理项缺少配对"));
                        int low = 0;
                        for (int i = 0; i < 4; ++i) {
                            const int digit = hexValue(m_input[m_pos + 2 + i]);
                            if (digit < 0) return fail(QStringLiteral("\\u 转义包含非十六进制字符"));
                            low = low * 16 + digit;
                        }
                        if (low < 0xDC00 || low > 0xDFFF)
                            return fail(QStringLiteral("高代理项配对无效"));
                        m_pos += 6;
                        const uint32_t scalar = 0x10000u
                            + (uint32_t(code - 0xD800) << 10) + uint32_t(low - 0xDC00);
                        out += QString::fromUcs4(&scalar, 1);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        return fail(QStringLiteral("孤立低代理项"));
                    } else {
                        out += QChar(ushort(code));
                    }
                    break;
                }
                default: return fail(QStringLiteral("非法字符串转义"));
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20)
                return fail(QStringLiteral("字符串包含未转义控制字符"));
            pending.append(c);
        }
        return fail(QStringLiteral("字符串未闭合"));
    }

    bool parseObject(Value &out, int depth)
    {
        if (!consume('{')) return false;
        out = Value::object();
        skipWhitespace();
        if (m_pos < m_input.size() && m_input[m_pos] == '}') {
            ++m_pos;
            return true;
        }
        while (true) {
            skipWhitespace();
            if (m_pos >= m_input.size() || m_input[m_pos] != '"')
                return fail(QStringLiteral("对象键必须是字符串"));
            QString key;
            if (!parseString(key)) return false;
            skipWhitespace();
            if (!consume(':')) return false;
            skipWhitespace();
            Value value;
            if (!parseValue(value, depth + 1)) return false;
            if (out.member(key)) return fail(QStringLiteral("对象包含重复键：%1").arg(key));
            out.insert(std::move(key), std::move(value));
            skipWhitespace();
            if (m_pos < m_input.size() && m_input[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            if (!consume(',')) return false;
            skipWhitespace();
            if (m_pos < m_input.size() && m_input[m_pos] == '}')
                return fail(QStringLiteral("对象不允许尾随逗号"));
        }
    }

    bool parseArray(Value &out, int depth)
    {
        if (!consume('[')) return false;
        out = Value::array();
        skipWhitespace();
        if (m_pos < m_input.size() && m_input[m_pos] == ']') {
            ++m_pos;
            return true;
        }
        while (true) {
            skipWhitespace();
            Value value;
            if (!parseValue(value, depth + 1)) return false;
            out.append(std::move(value));
            skipWhitespace();
            if (m_pos < m_input.size() && m_input[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            if (!consume(',')) return false;
            skipWhitespace();
            if (m_pos < m_input.size() && m_input[m_pos] == ']')
                return fail(QStringLiteral("数组不允许尾随逗号"));
        }
    }

    QByteArrayView m_input;
    ParseError *m_error = nullptr;
    int m_pos = 0;
};

} // namespace

Value::Value() = default;

Value::Value(bool value)
    : m_type(Type::Bool), m_bool(value)
{
}

Value Value::string(QString value)
{
    Value result;
    result.m_type = Type::String;
    result.m_text = std::move(value);
    return result;
}

Value Value::number(QString lexical)
{
    Value result;
    result.m_type = Type::Number;
    result.m_text = std::move(lexical);
    return result;
}

Value Value::array()
{
    Value result;
    result.m_type = Type::Array;
    return result;
}

Value Value::object()
{
    Value result;
    result.m_type = Type::Object;
    return result;
}

Value Value::null()
{
    return {};
}

const Value *Value::member(QStringView key) const
{
    if (m_type != Type::Object) return nullptr;
    for (size_t i = 0; i < m_keys.size(); ++i)
        if (m_keys[i] == key) return &m_values[i];
    return nullptr;
}

std::optional<qint64> Value::integer() const
{
    if (m_type != Type::Number) return std::nullopt;
    if (m_text.contains(QLatin1Char('.')) || m_text.contains(QLatin1Char('e'))
        || m_text.contains(QLatin1Char('E')))
        return std::nullopt;
    bool ok = false;
    const qint64 value = m_text.toLongLong(&ok, 10);
    if (!ok) return std::nullopt;
    return value;
}

std::optional<quint64> Value::unsignedInteger() const
{
    if (m_type != Type::Number) return std::nullopt;
    if (m_text.startsWith(QLatin1Char('-'))) return std::nullopt;
    if (m_text.contains(QLatin1Char('.')) || m_text.contains(QLatin1Char('e'))
        || m_text.contains(QLatin1Char('E')))
        return std::nullopt;
    bool ok = false;
    const quint64 value = m_text.toULongLong(&ok, 10);
    if (!ok) return std::nullopt;
    return value;
}

void Value::append(Value value)
{
    Q_ASSERT(m_type == Type::Array);
    m_array.push_back(std::move(value));
}

void Value::insert(QString key, Value value)
{
    Q_ASSERT(m_type == Type::Object);
    m_keys.push_back(std::move(key));
    m_values.push_back(std::move(value));
}

std::optional<Value> parse(QByteArrayView input, ParseError *error)
{
    if (error) *error = {};
    Parser parser(input, error);
    return parser.run();
}

void Writer::beforeValue()
{
    if (m_levels.empty()) return;
    auto &level = m_levels.back();
    if (!level.first) m_output += ',';
    level.first = false;
    if (level.object) {
        Q_ASSERT(m_hasPendingKey);
        appendString(m_pendingKey);
        m_output += ':';
        m_pendingKey.clear();
        m_hasPendingKey = false;
    }
}

void Writer::beginObject()
{
    beforeValue();
    m_output += '{';
    m_levels.push_back(Level{true, true});
}

void Writer::endObject()
{
    Q_ASSERT(!m_levels.empty() && m_levels.back().object);
    Q_ASSERT(!m_hasPendingKey);
    m_output += '}';
    m_levels.pop_back();
}

void Writer::beginArray()
{
    beforeValue();
    m_output += '[';
    m_levels.push_back(Level{false, true});
}

void Writer::endArray()
{
    Q_ASSERT(!m_levels.empty() && !m_levels.back().object);
    m_output += ']';
    m_levels.pop_back();
}

void Writer::key(QStringView key)
{
    Q_ASSERT(!m_levels.empty() && m_levels.back().object);
    Q_ASSERT(!m_hasPendingKey);
    m_pendingKey = key.toString();
    m_hasPendingKey = true;
}

void Writer::string(QStringView value)
{
    beforeValue();
    appendString(value);
}

void Writer::number(int value)
{
    beforeValue();
    m_output += QByteArray::number(value);
}

void Writer::number(qint64 value)
{
    beforeValue();
    m_output += QByteArray::number(value);
}

void Writer::number(quint64 value)
{
    beforeValue();
    m_output += QByteArray::number(value);
}

void Writer::boolean(bool value)
{
    beforeValue();
    m_output += value ? "true" : "false";
}

void Writer::null()
{
    beforeValue();
    m_output += "null";
}

QByteArray Writer::take()
{
    Q_ASSERT(m_levels.empty());
    Q_ASSERT(!m_hasPendingKey);
    return std::move(m_output);
}

void Writer::appendString(QStringView value)
{
    m_output += '"';
    appendEscapedUtf8(value.toString().toUtf8());
    m_output += '"';
}

void Writer::appendEscapedUtf8(const QByteArray &utf8)
{
    static constexpr char Hex[] = "0123456789abcdef";
    for (const char raw : utf8) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"': m_output += "\\\""; break;
        case '\\': m_output += "\\\\"; break;
        case '\b': m_output += "\\b"; break;
        case '\f': m_output += "\\f"; break;
        case '\n': m_output += "\\n"; break;
        case '\r': m_output += "\\r"; break;
        case '\t': m_output += "\\t"; break;
        default:
            if (c < 0x20) {
                m_output += "\\u00";
                m_output += Hex[c >> 4];
                m_output += Hex[c & 0x0F];
            } else {
                m_output += char(c);
            }
            break;
        }
    }
}

} // namespace vusiif::zto::desktop_share::json
