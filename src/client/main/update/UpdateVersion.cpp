#include "UpdateVersion.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace vusiif::zto::update {
namespace {

bool isNumeric(const QString &text)
{
    if (text.isEmpty()) return false;
    for (const QChar ch : text) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return false;
    }
    return true;
}

bool hasLeadingZero(const QString &text)
{
    return text.size() > 1 && text.startsWith(QLatin1Char('0'));
}

int compareText(const QString &left, const QString &right)
{
    const auto result = QString::compare(left, right, Qt::CaseSensitive);
    if (result == 0) return 0;
    return result < 0 ? -1 : 1;
}

} // namespace

QString Version::toString() const
{
    QString result = QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
    if (!prerelease.empty()) {
        QStringList parts;
        for (const auto &identifier : prerelease) {
            parts.append(identifier.kind == VersionIdentifier::Kind::Numeric
                             ? QString::number(identifier.number)
                             : identifier.text);
        }
        result += QLatin1Char('-') + parts.join(QLatin1Char('.'));
    }
    if (!buildMetadata.isEmpty()) result += QLatin1Char('+') + buildMetadata;
    return result;
}

std::optional<Version> parseVersion(const QString &text)
{
    static const QRegularExpression pattern(
        QStringLiteral("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)"
                       "(?:-([0-9A-Za-z.-]+))?(?:\\+([0-9A-Za-z.-]+))?$"));
    const auto match = pattern.match(text);
    if (!match.hasMatch()) return std::nullopt;

    Version version;
    bool ok = false;
    version.major = match.captured(1).toInt(&ok);
    if (!ok) return std::nullopt;
    version.minor = match.captured(2).toInt(&ok);
    if (!ok) return std::nullopt;
    version.patch = match.captured(3).toInt(&ok);
    if (!ok) return std::nullopt;
    if (version.major < 0 || version.minor < 0 || version.patch < 0) return std::nullopt;

    const auto prerelease = match.captured(4);
    if (!prerelease.isEmpty()) {
        const auto identifiers = prerelease.split(QLatin1Char('.'), Qt::KeepEmptyParts);
        for (const auto &value : identifiers) {
            if (value.isEmpty()) return std::nullopt;
            if (isNumeric(value)) {
                if (hasLeadingZero(value)) return std::nullopt;
                bool numberOk = false;
                const auto number = value.toULongLong(&numberOk);
                if (!numberOk) return std::nullopt;
                VersionIdentifier identifier;
                identifier.kind = VersionIdentifier::Kind::Numeric;
                identifier.number = number;
                version.prerelease.push_back(identifier);
            } else {
                // Alphanumerics are restricted to [0-9A-Za-z-] by the regex.
                VersionIdentifier identifier;
                identifier.kind = VersionIdentifier::Kind::Alphanumeric;
                identifier.text = value;
                version.prerelease.push_back(identifier);
            }
        }
    }
    version.buildMetadata = match.captured(5);
    return version;
}

int compareVersions(const Version &left, const Version &right)
{
    if (left.major != right.major) return left.major < right.major ? -1 : 1;
    if (left.minor != right.minor) return left.minor < right.minor ? -1 : 1;
    if (left.patch != right.patch) return left.patch < right.patch ? -1 : 1;

    // A version without prerelease identifiers has higher precedence.
    if (left.prerelease.empty() && right.prerelease.empty()) return 0;
    if (left.prerelease.empty()) return 1;
    if (right.prerelease.empty()) return -1;

    const auto count = std::min(left.prerelease.size(), right.prerelease.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto &a = left.prerelease[index];
        const auto &b = right.prerelease[index];
        if (a.kind == VersionIdentifier::Kind::Numeric && b.kind == VersionIdentifier::Kind::Numeric) {
            if (a.number != b.number) return a.number < b.number ? -1 : 1;
            continue;
        }
        // Numeric identifiers always have lower precedence than alphanumeric.
        if (a.kind == VersionIdentifier::Kind::Numeric) return -1;
        if (b.kind == VersionIdentifier::Kind::Numeric) return 1;
        const int result = compareText(a.text, b.text);
        if (result != 0) return result;
    }
    if (left.prerelease.size() == right.prerelease.size()) return 0;
    return left.prerelease.size() < right.prerelease.size() ? -1 : 1;
}

bool versionIsNewer(const QString &candidate, const QString &current, bool *parseError)
{
    const auto candidateVersion = parseVersion(candidate);
    const auto currentVersion = parseVersion(current);
    if (!candidateVersion || !currentVersion) {
        if (parseError) *parseError = true;
        return false;
    }
    if (parseError) *parseError = false;
    return compareVersions(*candidateVersion, *currentVersion) > 0;
}

} // namespace vusiif::zto::update
