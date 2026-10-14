#include "PluginPackage.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCommandLineParser>
#include <QTextStream>
#include <QFile>
int main(int argc,char **argv) {
    using namespace vusiif::ztgui::package;
    QCoreApplication app(argc,argv); QCommandLineParser parser; parser.addHelpOption();
    for(const auto &key:{"library","output","host","id","name","version","apiVersion","iid","bundle-manifest"})
        parser.addOption(QCommandLineOption(key,key,"value"));
    for(const auto &key:{"inspect","list","extract-licenses"}) parser.addOption(QCommandLineOption(key,key,"path"));
    parser.process(app);
    for(const auto &key:{"inspect","list","extract-licenses"}) if(parser.isSet(key)) {
        const auto p=inspect(parser.value(key)); QString error=p.error;
        if(error.isEmpty() && QString(key)=="extract-licenses") {
            if(!parser.isSet("output")) error="Missing --output directory";
            else error=extractLicenses(p,parser.value("output"));
        }
        if(!error.isEmpty()) { QTextStream(stderr)<<error<<'\n'; return 1; }
        if(QString(key)=="inspect") QTextStream(stdout)<<QJsonDocument(p.info).toJson(QJsonDocument::Indented);
        if(QString(key)=="list") QTextStream(stdout)<<QJsonDocument(p.info["files"].toArray()).toJson(QJsonDocument::Indented);
        return 0;
    }
    QJsonObject info;
    for(const auto &key:{"host","id","name","version","apiVersion","iid"}) {
        if(!parser.isSet(key)) { QTextStream(stderr)<<"Missing --"<<key<<'\n'; return 1; } info[key]=parser.value(key);
    }
    QString error;
    if(parser.isSet("bundle-manifest")) {
        QFile file(parser.value("bundle-manifest")); QJsonParseError parse;
        if(!file.open(QIODevice::ReadOnly)) error=file.errorString();
        else {
            const auto document=QJsonDocument::fromJson(file.readAll(),&parse);
            if(parse.error!=QJsonParseError::NoError || !document.isObject()) error="Invalid bundle manifest";
            else error=writeBundle(document.object(),parser.value("output"),info);
        }
    } else error=write(parser.value("library"),parser.value("output"),info);
    if(!error.isEmpty()) { QTextStream(stderr)<<error<<'\n'; return 1; } return 0;
}
