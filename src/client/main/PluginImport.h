#pragma once
#include <QString>
namespace vusiif::ztgui::plugins {
// Returns an error; success copies bytes only, never loads plugin code.
QString importPlugin(const QString &source, const QString &directory);
}
