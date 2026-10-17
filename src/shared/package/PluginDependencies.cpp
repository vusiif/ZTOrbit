#include "PluginDependencies.h"
#include "PluginPackage.h"
#include <ztprotocol/PluginIdentity.h>
#include <ztprotocol/ServiceWire.h>
#include <QJsonArray>
#include <QRegularExpression>
#include <array>
#include <functional>
namespace vusiif::ztgui::package {
static bool semver(const QString &s,std::array<quint64,3> &v) {
    static const QRegularExpression re("\\A(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\z");
    const auto m=re.match(s); if(!m.hasMatch()) return false;
    for(int i=0;i<3;++i) { bool ok=false; v[i]=m.captured(i+1).toULongLong(&ok); if(!ok) return false; } return true;
}
bool versionMatches(const QString &version,const QString &range,QString *error) {
    std::array<quint64,3> actual{}; if(!semver(version,actual)) { if(error) *error="Invalid semantic version: "+version; return false; }
    if(range.trimmed().isEmpty()) { if(error) *error="Empty version range"; return false; }
    bool matches=true;
    for(const auto &term:range.split(' ',Qt::SkipEmptyParts)) {
        static const QRegularExpression re("\\A(>=|<=|>|<|=)?([0-9]+\\.[0-9]+\\.[0-9]+)\\z");
        const auto m=re.match(term); std::array<quint64,3> expected{};
        if(!m.hasMatch() || !semver(m.captured(2),expected)) { if(error) *error="Unsupported version range: "+range; return false; }
        const auto op=m.captured(1);
        matches &= op==">="?actual>=expected:op=="<="?actual<=expected:op==">"?actual>expected:op=="<"?actual<expected:actual==expected;
    } return matches;
}
static QString servicesValid(const QJsonValue &value) {
    if(!value.isArray() || value.toArray().size()>64) return "Invalid service declarations";
    QSet<QString> seen;
    static const QRegularExpression id("\\A[a-z][a-z0-9._-]{0,127}\\z"),api("\\A[1-9][0-9]{0,8}\\z");
    for(const auto &v:value.toArray()) {
        const auto o=v.toObject(); const auto s=o["service"].toString();
        if(!v.isObject() || !id.match(s).hasMatch() || !api.match(o["api"].toString()).hasMatch() || seen.contains(s)) return "Invalid/duplicate service: "+s;
        seen.insert(s);
    } return {};
}
QString validateServiceManifest(const QJsonObject &info) {
    if(info["formatVersion"].toInt()<3) {
        if(info.contains("execution") || info.contains("dependencies") || info.contains("provides")) return "Service declarations require ZTPK3";
        return {};
    }
    if(info["host"]!="client") return "ZTPK3 services require the client dependency scheduler";
    const auto e=info["execution"].toObject(); const auto kind=e["kind"].toString();
    if((kind!="worker-process" && kind!="in-process") || e["entry"]!=info["entry"]) return "Invalid execution mode/entry";
    if(kind=="worker-process" && (info["host"]!="client" || e["protocol"].toString()!=protocol::service::Protocol)) return "Unsupported worker protocol/host";
    auto error=servicesValid(info["provides"]); if(!error.isEmpty()) return error;
    if(kind=="in-process" && !info["provides"].toArray().isEmpty()) return "Service providers must run in a worker process";
    if(!info["dependencies"].isArray() || info["dependencies"].toArray().size()>64) return "Invalid dependencies";
    QSet<QString> ids;
    for(const auto &v:info["dependencies"].toArray()) {
        const auto d=v.toObject(); const auto id=d["pluginId"].toString().toCaseFolded();
        if(!v.isObject() || !protocol::validPluginId(id) || ids.contains(id)) return "Invalid/duplicate dependency: "+id;
        ids.insert(id); QString rangeError; versionMatches("0.0.0",d["versionRange"].toString(),&rangeError);
        if(!rangeError.isEmpty()) return rangeError;
        error=servicesValid(d["services"]); if(!error.isEmpty()) return error;
    }
    std::array<quint64,3> version{}; if(!semver(info["version"].toString(),version)) return "ZTPK3 requires a semantic plugin version";
    return {};
}
DependencyPlan dependencyPlan(const QMap<QString,QJsonObject> &catalog,const QString &target,const QSet<QString> &disabled) {
    DependencyPlan result; QMap<QString,int> colors; QStringList chain;
    std::function<bool(QString)> visit=[&](QString id) {
        id=id.toCaseFolded(); chain.append(id);
        auto fail=[&](const QString &why) { result.error=chain.join(" -> ")+": "+why; chain.removeLast(); return false; };
        if(colors.value(id)==1) return fail("Dependency cycle");
        if(!catalog.contains(id)) return fail("Missing plugin");
        if(disabled.contains(id) && id!=target.toCaseFolded()) return fail("Dependency persistently disabled");
        if(colors.value(id)==2) { chain.removeLast(); return true; }
        colors[id]=1; const auto node=catalog[id]; const auto invalid=validateServiceManifest(node); if(!invalid.isEmpty()) return fail(invalid);
        for(const auto &v:node["dependencies"].toArray()) {
            const auto d=v.toObject(); const auto provider=d["pluginId"].toString().toCaseFolded();
            if(!catalog.contains(provider)) { chain.append(provider); return fail("Missing plugin"); }
            QString error;
            if(!versionMatches(catalog[provider]["version"].toString(),d["versionRange"].toString(),&error)) return fail(error.isEmpty()?"Incompatible dependency version: "+provider:error);
            for(const auto &s:d["services"].toArray()) if(!catalog[provider]["provides"].toArray().contains(s)) return fail("Missing service from "+provider+": "+s.toObject()["service"].toString());
            if(!visit(provider)) return false;
        }
        colors[id]=2; result.order.append(id); chain.removeLast(); return true;
    };
    visit(target); return result;
}
}
