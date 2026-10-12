#pragma once
#include <QString>
namespace vusiif::ztgui::util {
enum class LogLevel { Debug, Info, Warning, Error };
struct LogResult {
    QString hostError;
    QString pluginError;
    bool ok() const { return hostError.isEmpty() && pluginError.isEmpty(); }
};
// Borrowed until the plugin's destroy/stop hook returns. Thread-safe.
class PluginLogger {
public:
    virtual ~PluginLogger() = default;
    virtual LogResult write(LogLevel level, const QString &message) = 0;
};
inline QString logLevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return QStringLiteral("DEBUG");
    case LogLevel::Info: return QStringLiteral("INFO");
    case LogLevel::Warning: return QStringLiteral("WARN");
    case LogLevel::Error: return QStringLiteral("ERROR");
    }
    return QStringLiteral("INFO");
}
}
