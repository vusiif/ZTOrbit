#pragma once

#include <QByteArray>
#include <QString>

namespace vusiif::zto::update {

// Ed25519 detached signature verification through libsodium.
//
// libsodium is loaded at runtime (QLibrary) instead of being linked: the host
// already deploys libsodium.dll for the plugin package format, and keeping the
// update module link-neutral lets the isolated tests and the helper tooling
// share this translation unit. If the library cannot be loaded the verification
// fails closed: a check reports an error instead of trusting the download.
class UpdateCrypto {
public:
    static constexpr int SignatureBytes = 64;
    static constexpr int PublicKeyBytes = 32;

    // Base64 decode of the 32-byte public key. Fails for wrong length, invalid
    // alphabet or noncanonical padding.
    static bool decodePublicKey(const QString &base64, QByteArray &key, QString &error);

    // Verifies the detached signature over `message` exactly as received.
    static bool verifyDetached(const QByteArray &message, const QByteArray &signature,
                               const QByteArray &publicKey, QString &error);

    // Strict base64 decoding used for X-Z2-Signature.
    static bool decodeSignature(const QByteArray &base64, QByteArray &signature, QString &error);

    static QString backendName();
    static bool available(QString &error);
};

} // namespace vusiif::zto::update
