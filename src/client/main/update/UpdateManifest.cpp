#include "UpdateManifest.h"

#include "UpdateVersion.h"
#include "UpdaterPlan.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSysInfo>
#include <QUrl>

namespace vusiif::zto::update {
namespace {

bool isIdentifier(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9._-]*$"));
    return pattern.match(value).hasMatch();
}

bool isSha256(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(value).hasMatch();
}

bool validateArtifact(const UpdateArtifact &artifact, QString &error)
{
    if (!isIdentifier(artifact.platform)) {
        error = QStringLiteral("更新包的 platform 标识无效");
        return false;
    }
    if (!isIdentifier(artifact.arch)) {
        error = QStringLiteral("更新包的 arch 标识无效");
        return false;
    }
    if (!parseVersion(artifact.version)) {
        error = QStringLiteral("更新包的版本号不是有效的 SemVer：%1").arg(artifact.version);
        return false;
    }
    if (artifact.size <= 0 || artifact.size > MaximumPackageBytes) {
        error = QStringLiteral("更新包大小无效");
        return false;
    }
    if (!isSha256(artifact.sha256)) {
        error = QStringLiteral("更新包的 SHA-256 无效");
        return false;
    }
    const QUrl url(artifact.url, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()
        || (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http"))) {
        error = QStringLiteral("更新包地址必须是有效的 HTTP(S) URL");
        return false;
    }
    if (!url.userInfo().isEmpty()) {
        error = QStringLiteral("更新包地址不能内嵌凭据");
        return false;
    }
    if (url.hasFragment()) {
        error = QStringLiteral("更新包地址不能包含片段");
        return false;
    }
    for (const auto &removal : artifact.removeFiles) {
        std::string pathError;
        const auto utf8 = removal.toStdString();
        if (updater::normalizeRelativePath(utf8, pathError).empty()
            || !updater::isValidRelativePath(utf8, pathError)) {
            error = QStringLiteral("更新包旧文件列表包含不安全的路径：%1").arg(QString::fromStdString(pathError));
            return false;
        }
    }
    return true;
}

} // namespace

QString targetPlatform()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

QString targetArchitecture()
{
    const auto architecture = QSysInfo::buildCpuArchitecture();
    if (architecture == QLatin1String("x86_64") || architecture == QLatin1String("amd64"))
        return QStringLiteral("x64");
    if (architecture == QLatin1String("arm64") || architecture == QLatin1String("aarch64"))
        return QStringLiteral("arm64");
    if (architecture == QLatin1String("i386") || architecture == QLatin1String("x86"))
        return QStringLiteral("x86");
    return architecture.toLower();
}

bool parseManifest(const QByteArray &bytes, UpdateManifest &manifest, QString &error)
{
    manifest = UpdateManifest{};
    if (bytes.isEmpty()) {
        error = QStringLiteral("更新清单为空");
        return false;
    }
    if (bytes.size() > MaximumManifestBytes) {
        error = QStringLiteral("更新清单超过大小限制");
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        error = QStringLiteral("更新清单不是有效的 JSON：%1").arg(parseError.errorString());
        return false;
    }
    if (!document.isObject()) {
        error = QStringLiteral("更新清单必须是 JSON 对象");
        return false;
    }
    const auto object = document.object();
    if (object.value(QStringLiteral("schemaVersion")).toInt(-1) != 1) {
        error = QStringLiteral("不支持的更新清单 schemaVersion");
        return false;
    }
    manifest.schemaVersion = 1;
    if (object.contains(QStringLiteral("channel"))) {
        const auto channel = object.value(QStringLiteral("channel"));
        if (!channel.isString() || !isIdentifier(channel.toString())) {
            error = QStringLiteral("更新清单 channel 无效");
            return false;
        }
        manifest.channel = channel.toString();
    } else {
        manifest.channel = QStringLiteral("stable");
    }
    if (object.contains(QStringLiteral("publishedAt"))) {
        const auto published = object.value(QStringLiteral("publishedAt"));
        if (!published.isString()
            || !QDateTime::fromString(published.toString(), Qt::ISODate).isValid()) {
            error = QStringLiteral("更新清单 publishedAt 不是 ISO-8601 时间");
            return false;
        }
        manifest.publishedAt = published.toString();
    }
    const auto minimum = object.value(QStringLiteral("minUpdatableVersion"));
    if (!minimum.isString() || !parseVersion(minimum.toString())) {
        error = QStringLiteral("更新清单缺少有效的 minUpdatableVersion");
        return false;
    }
    manifest.minUpdatableVersion = minimum.toString();

    const auto artifacts = object.value(QStringLiteral("artifacts"));
    if (!artifacts.isArray() || artifacts.toArray().isEmpty()) {
        error = QStringLiteral("更新清单缺少 artifacts 数组");
        return false;
    }
    if (artifacts.toArray().size() > 64) {
        error = QStringLiteral("更新清单 artifacts 数量过多");
        return false;
    }
    QSet<QString> combinations;
    for (const auto &value : artifacts.toArray()) {
        if (!value.isObject()) {
            error = QStringLiteral("更新清单 artifacts 条目必须是对象");
            return false;
        }
        const auto entry = value.toObject();
        UpdateArtifact artifact;
        artifact.platform = entry.value(QStringLiteral("platform")).toString();
        artifact.arch = entry.value(QStringLiteral("arch")).toString();
        artifact.version = entry.value(QStringLiteral("version")).toString();
        artifact.url = entry.value(QStringLiteral("url")).toString();
        artifact.size = static_cast<qint64>(entry.value(QStringLiteral("size")).toDouble(-1));
        artifact.sha256 = entry.value(QStringLiteral("sha256")).toString();
        const auto removals = entry.value(QStringLiteral("removeFiles"));
        if (!removals.isUndefined()) {
            if (!removals.isArray()) {
                error = QStringLiteral("更新清单 removeFiles 必须是数组");
                return false;
            }
            for (const auto &removal : removals.toArray()) {
                if (!removal.isString()) {
                    error = QStringLiteral("更新清单 removeFiles 条目必须是字符串");
                    return false;
                }
                artifact.removeFiles.append(removal.toString());
            }
        }
        if (!validateArtifact(artifact, error)) return false;
        const auto key = artifact.platform + QLatin1Char('/') + artifact.arch;
        if (combinations.contains(key)) {
            error = QStringLiteral("更新清单包含重复的 platform/arch 条目");
            return false;
        }
        combinations.insert(key);
        manifest.artifacts.append(artifact);
    }
    return true;
}

bool selectArtifact(const UpdateManifest &manifest, const QString &platform, const QString &arch,
                    UpdateArtifact &artifact, QString &error)
{
    for (const auto &candidate : manifest.artifacts) {
        if (candidate.platform.compare(platform, Qt::CaseInsensitive) == 0
            && candidate.arch.compare(arch, Qt::CaseInsensitive) == 0) {
            artifact = candidate;
            return true;
        }
    }
    QStringList available;
    for (const auto &candidate : manifest.artifacts)
        available.append(candidate.platform + QLatin1Char('/') + candidate.arch);
    error = QStringLiteral("更新清单没有适用于 %1/%2 的安装包（清单提供：%3）")
                .arg(platform, arch, available.join(QStringLiteral(", ")));
    return false;
}

} // namespace vusiif::zto::update
