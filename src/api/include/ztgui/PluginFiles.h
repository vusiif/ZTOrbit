#pragma once

#include <QByteArray>
#include <QString>

namespace vusiif::ztgui::util {
struct FileResult {
    QByteArray data;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// Thread-safe binary IO relative to applicationDirPath()/plugins-data/<PluginInfo.name>/.
// The host adds the plugin directory; callers pass only paths within it.
// Keep names stable and unique: renaming changes the directory, equal names share it.
// Paths must be relative, without '..', drive prefixes or symbolic links.
class PluginFiles {
public:
    virtual ~PluginFiles() = default;
    virtual FileResult readFile(const QString &relativePath) = 0;
    // Atomically replaces the file and creates missing parent directories.
    virtual FileResult writeFile(const QString &relativePath, const QByteArray &data) = 0;
};
}
