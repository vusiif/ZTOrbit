#include "PluginPackage.h"
#include <ztserver/PluginInterface.h>
#include <QFile>
#include <QJsonArray>
#include <QtTest>
namespace vusiif::ztgui::package { QString writeLegacy(const QString &,const QString &,QJsonObject); }
using namespace vusiif::ztgui::package;
class ServerPackageTests : public QObject {
    Q_OBJECT
private slots:
    void formatsAndMultipleFiles() {
        QTemporaryDir temp; const auto path=temp.filePath("server.ztplugin"),license=temp.filePath("license.txt");
        QFile f(license); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("license"); f.close();
        QJsonObject info{{"host","server"},{"id","test-echo"},{"name","test-echo"},{"version","1"},{"apiVersion",QString::fromLatin1(vusiif::ztgui::server::api::ApiVersion)},{"iid","vusiif.ztgui.server.api.PluginInterface/2.0"}};
        const QJsonObject bundle{{"files",QJsonArray{
            QJsonObject{{"path","plugin/plugin.so"},{"role","entry"},{"source",QString(FIXTURE_LIBRARY)}},
            QJsonObject{{"path","licenses/license.txt"},{"role","license"},{"source",license}}
        }}};
        auto error=writeBundle(bundle,path,info); QVERIFY2(error.isEmpty(),qPrintable(error));
        auto p=inspect(path); QVERIFY2(p.ok(),qPrintable(p.error));
        QVERIFY(validate(p.info,"server",vusiif::ztgui::server::api::ApiVersion,"vusiif.ztgui.server.api.PluginInterface/2.0").isEmpty());
        std::unique_ptr<QTemporaryDir> directory; std::unique_ptr<QLockFile> lock; QString library;
        QVERIFY(extract(p,directory,library,lock).isEmpty()); QVERIFY(QFileInfo(library).isFile());
        QVERIFY(extractLicenses(p,temp.filePath("export")).isEmpty()); QVERIFY(QFileInfo(temp.filePath("export/licenses/license.txt")).isFile());
        QFile tamper(path); QVERIFY(tamper.open(QIODevice::Append)); tamper.write("extra"); tamper.close(); QVERIFY(!inspect(path).ok());
        error=writeLegacy(FIXTURE_LIBRARY,path,info); QVERIFY2(error.isEmpty(),qPrintable(error));
        p=inspect(path); QVERIFY(p.ok()); QCOMPARE(p.info["formatVersion"].toInt(),1);
        lock.reset(); discardCache(directory); QVERIFY(extract(p,directory,library,lock).isEmpty());
    }
};
QTEST_GUILESS_MAIN(ServerPackageTests)
#include "PackageTests.moc"
