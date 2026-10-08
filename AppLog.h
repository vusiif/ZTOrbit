#pragma once
#include <QString>

namespace AppLog {
QString path();
void initialize();
void write(const QString &text);
}
