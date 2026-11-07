#include "ResetPlan.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace vusiif::zto::reset {
namespace {

bool linkLike(const QFileInfo &info)
{
    return info.isSymLink() || info.isJunction();
}

EntryKind classify(const QFileInfo &info)
{
    if (linkLike(info)) return EntryKind::Link;
    if (info.isDir()) return EntryKind::Directory;
    if (info.isFile()) return EntryKind::File;
    return EntryKind::Other;
}

bool isStateDirectory(const QString &name)
{
    return name == QStringLiteral("networks.d")
        || name == QStringLiteral("peers.d")
        || name == QStringLiteral("moons.d");
}

// A path that can be safely combined with the data directory. The scanner
// always produces such paths; rejecting anything else here means a corrupted
// or hostile listing can never turn into a deletion candidate.
bool safeRelativePath(const QString &path)
{
    if (path.isEmpty() || path.size() > 1024) return false;
    if (path.contains(QLatin1Char('\\'))) return false;
    if (path.startsWith(QLatin1Char('/')) || QDir::isAbsolutePath(path)) return false;
    if (QRegularExpression(QStringLiteral("^[A-Za-z]:")).match(path).hasMatch()) return false;
    const auto parts = path.split(QLatin1Char('/'));
    for (const auto &part : parts) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")) return false;
    }
    return true;
}

const QList<QRegularExpression> &whitelistExpressions()
{
    static const QList<QRegularExpression> expressions = [] {
        QList<QRegularExpression> result;
        for (const auto &pattern : deletionWhitelist()) {
            QRegularExpression expression(pattern);
            expression.optimize();
            result.append(expression);
        }
        return result;
    }();
    return expressions;
}

void appendSorted(QStringList &output, QSet<QString> &seen, QStringList values)
{
    std::sort(values.begin(), values.end());
    for (const auto &value : values) {
        if (seen.contains(value)) continue;
        seen.insert(value);
        output.append(value);
    }
}

} // namespace

QStringList deletionWhitelist()
{
    // Verified against ZeroTier One's state-object names in service/OneService.cpp
    // (ZT_STATE_OBJECT_*): networks.d/<16 hex>.conf, peers.d/<10 hex>.peer,
    // moons.d/<16 hex>.moon and the optional "planet" root override file.
    // networks.d/<16 hex>.local.conf is documented per-network local settings.
    return {
        QStringLiteral("^networks\\.d/[0-9a-f]{16}\\.conf$"),
        QStringLiteral("^networks\\.d/[0-9a-f]{16}\\.local\\.conf$"),
        QStringLiteral("^peers\\.d/[0-9a-f]{10}\\.peer$"),
        QStringLiteral("^moons\\.d/[0-9a-f]{16}\\.moon$"),
        QStringLiteral("^planet$")
    };
}

bool whitelistedDeletion(const QString &relativePath)
{
    if (!safeRelativePath(relativePath)) return false;
    for (const auto &expression : whitelistExpressions())
        if (expression.match(relativePath).hasMatch()) return true;
    return false;
}

Snapshot scanDataDirectory(const QString &root)
{
    Snapshot snapshot;
    const QFileInfo rootInfo(root);
    if (!rootInfo.exists()) {
        snapshot.error = QStringLiteral("ZeroTier 数据目录不存在：%1").arg(QDir::toNativeSeparators(root));
        return snapshot;
    }
    if (linkLike(rootInfo)) {
        snapshot.error = QStringLiteral("ZeroTier 数据目录是链接或重解析点，拒绝清理：%1")
                             .arg(QDir::toNativeSeparators(root));
        return snapshot;
    }
    if (!rootInfo.isDir()) {
        snapshot.error = QStringLiteral("ZeroTier 数据目录不是目录：%1").arg(QDir::toNativeSeparators(root));
        return snapshot;
    }

    const QDir directory(root);
    // Hidden/System also lists dot files and reparse points; links are reported
    // as Link and never traversed.
    const auto filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
    const auto rootEntries = directory.entryInfoList(filters);
    for (const auto &info : rootEntries) {
        const auto kind = classify(info);
        snapshot.entries.append({info.fileName(), kind});
        if (kind != EntryKind::Directory || !isStateDirectory(info.fileName())) continue;
        const QDir stateDirectory(directory.filePath(info.fileName()));
        for (const auto &child : stateDirectory.entryInfoList(filters))
            snapshot.entries.append({info.fileName() + QLatin1Char('/') + child.fileName(), classify(child)});
    }
    return snapshot;
}

LocalConfigMerge mergeLocalConfig(const QByteArray &bytes, bool exists)
{
    LocalConfigMerge merge;
    merge.content = bytes;
    if (!exists) return merge;

    QByteArray json = bytes;
    if (json.startsWith("\xEF\xBB\xBF")) json.remove(0, 3);
    // ZeroTier accepts an empty local.conf and simply keeps the defaults.
    if (json.trimmed().isEmpty()) return merge;

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        merge.ok = false;
        merge.error = QStringLiteral("local.conf 不是合法 JSON：%1（偏移 %2）")
                          .arg(parseError.errorString()).arg(parseError.offset);
        return merge;
    }
    if (!document.isObject()) {
        merge.ok = false;
        merge.error = QStringLiteral("local.conf 根值必须是 JSON 对象。");
        return merge;
    }
    auto root = document.object();
    if (root.contains(QStringLiteral("settings")) && !root.value(QStringLiteral("settings")).isObject()) {
        merge.ok = false;
        merge.error = QStringLiteral("local.conf 的 settings 必须是 JSON 对象。");
        return merge;
    }
    auto settings = root.value(QStringLiteral("settings")).toObject();
    if (!settings.contains(QStringLiteral("tcpFallbackRelay"))) return merge;

    // Verified: no other local.conf key is derived from Moon or peer state.
    // The fallback switches and every unknown field stay untouched.
    settings.remove(QStringLiteral("tcpFallbackRelay"));
    root[QStringLiteral("settings")] = settings;
    merge.changed = true;
    merge.content = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return merge;
}

Plan buildPlan(const Snapshot &snapshot, const QByteArray &localConfig, bool localConfigExists)
{
    Plan plan;
    if (!snapshot.valid()) {
        plan.error = snapshot.error;
        return plan;
    }

    // Reject any listing entry that could not be combined safely with the data
    // directory, even if it would not match the whitelist. The scanner cannot
    // produce such a path, so this only triggers on corruption or injection.
    for (const auto &entry : snapshot.entries) {
        if (!safeRelativePath(entry.path)) {
            plan.error = QStringLiteral("数据目录列表包含不安全路径，拒绝清理：%1").arg(entry.path);
            return plan;
        }
    }

    QHash<QString, EntryKind> rootKinds;
    for (const auto &entry : snapshot.entries) {
        if (!entry.path.contains(QLatin1Char('/'))) rootKinds.insert(entry.path, entry.kind);
    }
    for (const auto &name : {QStringLiteral("networks.d"), QStringLiteral("peers.d"), QStringLiteral("moons.d")}) {
        if (!rootKinds.contains(name)) continue;
        const auto kind = rootKinds.value(name);
        if (kind == EntryKind::Link) {
            plan.error = QStringLiteral("%1 是链接或重解析点，拒绝清理。").arg(name);
            return plan;
        }
        if (kind != EntryKind::Directory) {
            plan.error = QStringLiteral("%1 不是目录，拒绝清理。").arg(name);
            return plan;
        }
    }
    if (rootKinds.value(QStringLiteral("planet")) == EntryKind::Link) {
        plan.error = QStringLiteral("planet 是链接或重解析点，拒绝清理。");
        return plan;
    }

    QStringList networks;
    QStringList peers;
    QStringList moons;
    bool planet = false;
    for (const auto &entry : snapshot.entries) {
        const auto parts = entry.path.split(QLatin1Char('/'));
        if (parts.size() == 1) {
            if (entry.path == QStringLiteral("planet") && entry.kind == EntryKind::File) planet = true;
            continue;
        }
        if (parts.size() != 2) continue;
        const auto &parent = parts.first();
        if (!isStateDirectory(parent)) continue;
        if (entry.kind == EntryKind::Link) {
            // A whitelisted candidate that is a link must never be followed.
            if (whitelistedDeletion(entry.path)) {
                plan.error = QStringLiteral("清理目标 %1 是链接或重解析点，拒绝清理。").arg(entry.path);
                return plan;
            }
            continue;
        }
        if (entry.kind != EntryKind::File) continue;
        if (!whitelistedDeletion(entry.path)) continue; // Unknown files are preserved.
        if (parent == QStringLiteral("networks.d")) networks.append(entry.path);
        else if (parent == QStringLiteral("peers.d")) peers.append(entry.path);
        else moons.append(entry.path);
    }

    QSet<QString> seen;
    appendSorted(plan.removals, seen, networks);
    appendSorted(plan.removals, seen, peers);
    appendSorted(plan.removals, seen, moons);
    if (planet && !seen.contains(QStringLiteral("planet"))) plan.removals.append(QStringLiteral("planet"));

    const auto merge = mergeLocalConfig(localConfig, localConfigExists);
    if (!merge.ok) {
        plan.error = merge.error;
        plan.removals.clear(); // A rejected merge must never yield a partial plan.
        return plan;
    }
    plan.originalLocalConfig = localConfig;
    plan.mergedLocalConfig = merge.content;
    plan.localConfigChanged = merge.changed;
    return plan;
}

} // namespace vusiif::zto::reset
