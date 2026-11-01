#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringView>

#include <cstdint>
#include <optional>
#include <vector>

namespace vusiif::zto::desktop_share::json {

// Minimal JSON value used by the desktop-share wire codec. It deliberately
// keeps object member order and the lexical form of numbers: the control
// contract compares encoded messages byte for byte across C++ and Java, so the
// codec cannot rely on a map that reorders keys or on a double round-trip.
class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value();
    explicit Value(bool value);
    static Value string(QString value);
    static Value number(QString lexical);
    static Value array();
    static Value object();
    static Value null();

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool boolean() const { return m_bool; }
    const QString &text() const { return m_text; }
    const std::vector<Value> &elements() const { return m_array; }
    const std::vector<QString> &keys() const { return m_keys; }
    const std::vector<Value> &values() const { return m_values; }
    qsizetype objectSize() const { return qsizetype(m_keys.size()); }

    const Value *member(QStringView key) const;

    // Strict decimal integer. Rejects fractional/exponent lexical forms.
    std::optional<qint64> integer() const;
    std::optional<quint64> unsignedInteger() const;

    void append(Value value);
    void insert(QString key, Value value);

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    QString m_text;
    std::vector<Value> m_array;
    std::vector<QString> m_keys;
    std::vector<Value> m_values;
};

struct ParseError {
    int offset = 0;
    QString message;
};

// RFC 8259 JSON, UTF-8 only, no comments or trailing content. Duplicate keys
// and nesting deeper than 64 levels are rejected.
std::optional<Value> parse(QByteArrayView input, ParseError *error = nullptr);

class Writer {
public:
    Writer() = default;

    void beginObject();
    void endObject();
    void beginArray();
    void endArray();
    void key(QStringView key);
    void string(QStringView value);
    void number(int value);
    void number(qint64 value);
    void number(quint64 value);
    void boolean(bool value);
    void null();

    QByteArray take();

private:
    struct Level {
        bool object = false;
        bool first = true;
    };

    void beforeValue();
    void appendString(QStringView value);
    void appendEscapedUtf8(const QByteArray &utf8);

    QByteArray m_output;
    std::vector<Level> m_levels;
    QString m_pendingKey;
    bool m_hasPendingKey = false;
};

} // namespace vusiif::zto::desktop_share::json
