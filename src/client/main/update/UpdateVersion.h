#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace vusiif::zto::update {

// One identifier of a SemVer 2.0.0 version. Build metadata ('+...') is ignored
// for ordering, as the specification requires.
struct VersionIdentifier {
    enum class Kind { Numeric, Alphanumeric };
    Kind kind = Kind::Numeric;
    quint64 number = 0;
    QString text;
};

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::vector<VersionIdentifier> prerelease;
    QString buildMetadata;

    bool isPrerelease() const { return !prerelease.empty(); }
    QString toString() const;
};

// Parses `MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]`. Returns std::nullopt for
// malformed input (empty identifiers, leading zeroes in numeric prerelease
// identifiers, illegal characters). A leading 'v' is deliberately rejected so
// callers cannot silently accept non-SemVer tags.
std::optional<Version> parseVersion(const QString &text);

// SemVer precedence: <0 when left is older, 0 when equal, >0 when newer.
// Build metadata never affects the result.
int compareVersions(const Version &left, const Version &right);

// Convenience wrapper used by the update decision. Returns false when either
// side cannot be parsed.
bool versionIsNewer(const QString &candidate, const QString &current, bool *parseError = nullptr);

} // namespace vusiif::zto::update
