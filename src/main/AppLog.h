#pragma once
#include <QString>

namespace AppLog {
// One UTF-8 log per process in <application directory>/logs/YYMMDD-N.log.
// The path stays fixed for the lifetime of the process, including across midnight.
QString path();
void initialize();
void write(const QString &text);
}
