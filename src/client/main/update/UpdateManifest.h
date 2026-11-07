#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace vusiif::zto::update {

// Upper bound of a manifest file; a small document keeps the signed payload and
// the parser cheap and bounds memory use during a check.
constexpr qint64 MaximumManifestBytes = 1024 * 1024;
constexpr qint64 MaximumPackageBytes = 512LL * 1024 * 1024;

struct UpdateArtifact {
    QString platform;
    QString arch;
    QString version;
    QString url;
    qint64 size = -1;
    QString sha256;
    // Explicit whitelist of release files removed by the update. Paths are
    // validated with the same rules as package entries (relative, no traversal,
    // never host-owned data).
    QStringList removeFiles;
};

struct UpdateManifest {
    int schemaVersion = 0;
    QString channel;
    QString publishedAt;
    QString minUpdatableVersion;
    QList<UpdateArtifact> artifacts;
};

// Parses and validates the manifest body. The signature (verified separately
// over the exact bytes returned here) is not part of the document.
bool parseManifest(const QByteArray &bytes, UpdateManifest &manifest, QString &error);

// Current platform/architecture identifiers used by the manifest.
QString targetPlatform();
QString targetArchitecture();

// Selects the single artifact matching platform/arch. Returns false with a
// human-readable reason when the manifest has no compatible package.
bool selectArtifact(const UpdateManifest &manifest, const QString &platform, const QString &arch,
                    UpdateArtifact &artifact, QString &error);

} // namespace vusiif::zto::update
