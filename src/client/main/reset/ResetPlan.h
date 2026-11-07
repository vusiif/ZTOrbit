#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// Pure decision logic for "reset local ZeroTier state".
//
// The plan builder never touches the filesystem itself: it receives a snapshot
// produced by scanDataDirectory() (or by tests) plus the current local.conf
// bytes, and returns the ordered delete list and the merged local.conf. The
// elevated script re-validates every path before deleting it.
namespace vusiif::zto::reset {

enum class EntryKind { File, Directory, Link, Other };

struct Entry {
    QString path; // '/'-separated, relative to the ZeroTier data directory
    EntryKind kind = EntryKind::Other;
};

struct Snapshot {
    QList<Entry> entries;
    QString error; // Non-empty means the directory could not be inspected safely.
    bool valid() const { return error.isEmpty(); }
};

// Enumerates the data directory root plus exactly one level below networks.d,
// peers.d and moons.d. Reparse points (symbolic links/junctions) are reported
// as Link and are never traversed.
Snapshot scanDataDirectory(const QString &root);

// The complete delete whitelist. The patterns are anchored regular expressions
// over '/'-separated paths relative to the data directory.
QStringList deletionWhitelist();

// True only for a path that is safe (relative, no traversal, no backslashes)
// and matches one of deletionWhitelist().
bool whitelistedDeletion(const QString &relativePath);

struct LocalConfigMerge {
    bool ok = true;
    bool changed = false;
    QByteArray content; // Merged bytes when changed; otherwise the input bytes.
    QString error;
};

// Removes only settings.tcpFallbackRelay. All other fields, including unknown
// fields and both TCP fallback switches, are preserved. Structurally invalid
// JSON is rejected instead of rewritten.
LocalConfigMerge mergeLocalConfig(const QByteArray &bytes, bool exists);

struct Plan {
    QString error;
    QStringList removals; // Ordered: networks, peers, moons, planet.
    QByteArray originalLocalConfig;
    QByteArray mergedLocalConfig;
    bool localConfigChanged = false;

    bool valid() const { return error.isEmpty(); }
    bool empty() const { return removals.isEmpty() && !localConfigChanged; }
};

// Validates the snapshot boundaries and produces the ordered plan. Any unsafe
// candidate (absolute path, traversal, link/junction, non-directory state
// parent, non-regular planet file) makes the whole plan invalid so that no
// deletion is attempted.
Plan buildPlan(const Snapshot &snapshot, const QByteArray &localConfig, bool localConfigExists);

} // namespace vusiif::zto::reset
