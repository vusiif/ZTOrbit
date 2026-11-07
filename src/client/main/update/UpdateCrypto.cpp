#include "UpdateCrypto.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibrary>

#include <mutex>

namespace vusiif::zto::update {
namespace {

using SodiumInit = int (*)();
using VerifyDetached = int (*)(const unsigned char *, const unsigned char *, unsigned long long,
                               const unsigned char *);

struct Backend {
    bool tried = false;
    bool ok = false;
    QString error;
    QString name;
    VerifyDetached verify = nullptr;
};

Backend &backend()
{
    static Backend instance;
    static std::once_flag once;
    std::call_once(once, [] {
        instance.tried = true;
        QStringList candidates;
#ifdef Q_OS_WIN
        candidates << QStringLiteral("libsodium")
                   << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("libsodium.dll"));
#elif defined(Q_OS_MACOS)
        candidates << QStringLiteral("libsodium")
                   << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("libsodium.dylib"));
#else
        candidates << QStringLiteral("sodium") << QStringLiteral("libsodium");
#endif
        for (const auto &candidate : candidates) {
            auto *library = new QLibrary(candidate);
            library->setLoadHints(QLibrary::ResolveAllSymbolsHint);
            if (!library->load()) {
                delete library;
                continue;
            }
            auto init = reinterpret_cast<SodiumInit>(library->resolve("sodium_init"));
            auto verify = reinterpret_cast<VerifyDetached>(library->resolve("crypto_sign_verify_detached"));
            if (!init || !verify) {
                library->unload();
                delete library;
                continue;
            }
            init();
            instance.verify = verify;
            instance.ok = true;
            instance.name = QStringLiteral("libsodium (%1)").arg(candidate);
            return;
        }
        instance.error = QStringLiteral("签名验证库不可用（未找到 libsodium）");
    });
    return instance;
}

bool isBase64Text(const QByteArray &text)
{
    for (const char ch : text) {
        const bool allowed = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
            || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' || ch == '='
            || ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t';
        if (!allowed) return false;
    }
    return true;
}

bool decodeBase64(const QByteArray &input, QByteArray &output, QString &error)
{
    QByteArray text = input;
    text.replace("\r", "").replace("\n", "").replace(" ", "").replace("\t", "");
    if (text.isEmpty()) {
        error = QStringLiteral("签名为空");
        return false;
    }
    if (!isBase64Text(text) || text.size() % 4 != 0) {
        error = QStringLiteral("签名不是有效的 Base64");
        return false;
    }
    output = QByteArray::fromBase64(text, QByteArray::AbortOnBase64DecodingErrors);
    if (output.isEmpty()) {
        error = QStringLiteral("签名不是有效的 Base64");
        return false;
    }
    return true;
}

} // namespace

QString UpdateCrypto::backendName()
{
    const auto &state = backend();
    return state.ok ? state.name : QStringLiteral("unavailable");
}

bool UpdateCrypto::available(QString &error)
{
    const auto &state = backend();
    if (!state.ok) {
        error = state.error;
        return false;
    }
    error.clear();
    return true;
}

bool UpdateCrypto::decodePublicKey(const QString &base64, QByteArray &key, QString &error)
{
    QByteArray decoded;
    if (!decodeBase64(base64.toLatin1(), decoded, error)) return false;
    if (decoded.size() != PublicKeyBytes) {
        error = QStringLiteral("Ed25519 公钥长度必须是 32 字节");
        return false;
    }
    key = decoded;
    return true;
}

bool UpdateCrypto::decodeSignature(const QByteArray &base64, QByteArray &signature, QString &error)
{
    QByteArray decoded;
    if (!decodeBase64(base64, decoded, error)) return false;
    if (decoded.size() != SignatureBytes) {
        error = QStringLiteral("Ed25519 签名长度必须是 64 字节");
        return false;
    }
    signature = decoded;
    return true;
}

bool UpdateCrypto::verifyDetached(const QByteArray &message, const QByteArray &signature,
                                  const QByteArray &publicKey, QString &error)
{
    const auto &state = backend();
    if (!state.ok) {
        error = state.error;
        return false;
    }
    if (signature.size() != SignatureBytes) {
        error = QStringLiteral("Ed25519 签名长度无效");
        return false;
    }
    if (publicKey.size() != PublicKeyBytes) {
        error = QStringLiteral("Ed25519 公钥长度无效");
        return false;
    }
    if (message.isEmpty()) {
        error = QStringLiteral("签名内容为空");
        return false;
    }
    const int result = state.verify(
        reinterpret_cast<const unsigned char *>(signature.constData()),
        reinterpret_cast<const unsigned char *>(message.constData()),
        static_cast<unsigned long long>(message.size()),
        reinterpret_cast<const unsigned char *>(publicKey.constData()));
    if (result != 0) {
        error = QStringLiteral("更新清单签名验证失败");
        return false;
    }
    error.clear();
    return true;
}

} // namespace vusiif::zto::update
