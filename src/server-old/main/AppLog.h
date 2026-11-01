#pragma once
#include <QString>

namespace vusiif::zto::util::log {
// One UTF-8 log per process in <application directory>/logs/YYMMDD-N.log.
// The path stays fixed for the lifetime of the process, including across midnight.
QString path();
void initialize();
void write(const QString &text);
QString writeChecked(const QString &text);
}
