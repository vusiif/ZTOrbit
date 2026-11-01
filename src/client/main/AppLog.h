#pragma once
#include <QString>
#include <functional>

namespace vusiif::zto::util::log {
// One UTF-8 log per process in <log directory>/YYMMDD-N.log.
// The path stays fixed for the lifetime of the process, including across midnight.
QString path();
// Selects the log directory before the first write. The default is
// <application directory>/logs. Ignored after the log has been opened.
void setDirectory(const QString &directory);
QString directory();
void initialize();
void write(const QString &text);
QString writeChecked(const QString &text);
// Called after a full log file was rotated and a new one opened. The callback
// runs without the log mutex held and may run on the writing thread; it must
// only schedule work. Passing an empty callback removes it.
void setRotationCallback(std::function<void()> callback);
// Host-side bookkeeping for log retention. Active files are never deleted.
void registerActiveFile(const QString &path);
void unregisterActiveFile(const QString &path);
bool isActiveFile(const QString &path);
qint64 maximumFileBytes();
}
