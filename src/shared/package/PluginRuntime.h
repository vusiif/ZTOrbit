#pragma once
#include "PluginPackage.h"
namespace vusiif::ztgui::package {
// Host private; dependencies outlive every business plugin and QApplication.
QJsonObject hostRuntimeContract();
QString validateRuntime(const PackageResult &);
QString activateRuntime(const PackageResult &,const QString &businessDirectory);
QStringList windowsImports(const QString &path,QString *error=nullptr);
QJsonObject runtimeStatistics();
bool businessLibraryMapped(const QString &path);
}
