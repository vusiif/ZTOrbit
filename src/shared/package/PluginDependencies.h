#pragma once
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QStringList>
namespace vusiif::zto::package {
QString validateServiceManifest(const QJsonObject &info);
bool versionMatches(const QString &version, const QString &range, QString *error = nullptr);
struct DependencyPlan { QStringList order; QString error; bool ok() const { return error.isEmpty(); } };
// The catalog must contain fully authenticated packages, keyed by case-folded ID.
DependencyPlan dependencyPlan(const QMap<QString,QJsonObject> &catalog, const QString &target,
                              const QSet<QString> &disabled = {});
}
