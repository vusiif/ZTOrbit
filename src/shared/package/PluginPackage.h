#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QLockFile>
#include <memory>
namespace vusiif::ztgui::package {
struct PackageResult {
    QJsonObject info;
    QString error;
    QByteArray header, encrypted; // No ciphertext is buffered by inspect (including v1).
    QString path;
    bool ok() const { return error.isEmpty(); }
};
PackageResult inspect(const QString &path);
// v2/v3 inspect already authenticates every block. Legacy v1 must also be
// authenticated before its declarations enter an executable dependency plan.
QString authenticate(const PackageResult &package);
QString validate(const QJsonObject &, const QString &host, const QString &api, const QString &iid);
QString write(const QString &library, const QString &output, QJsonObject info);
QString writeBundle(const QJsonObject &manifest, const QString &output, QJsonObject info);
QString validBundle(const QJsonObject &info);
QString safeRelativePath(const QString &path);
QString extractLicenses(const PackageResult &, const QString &destination);
QString extract(const PackageResult &, std::unique_ptr<QTemporaryDir> &directory, QString &library, std::unique_ptr<QLockFile> &lock);
QJsonObject buildInfo();
void cleanStaleCache();
void discardCache(std::unique_ptr<QTemporaryDir> &directory);
QString createCache(const QString &kind, std::unique_ptr<QTemporaryDir> &, std::unique_ptr<QLockFile> &);
}
