#include <QtTest>

#include "ResetPlan.h"
#include "ResetScript.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace vusiif::zto::reset;

namespace {

Entry fileEntry(const QString &path) { return {path, EntryKind::File}; }
Entry dirEntry(const QString &path) { return {path, EntryKind::Directory}; }
Entry linkEntry(const QString &path) { return {path, EntryKind::Link}; }

Snapshot snapshotOf(const QList<Entry> &entries)
{
    Snapshot snapshot;
    snapshot.entries = entries;
    return snapshot;
}

QByteArray bytesOf(const QString &text) { return text.toUtf8(); }

void writeFile(const QString &path, const QByteArray &bytes = QByteArray("x"))
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

QString sha256Hex(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

void writeRemovalList(const QString &path, const QStringList &removals)
{
    auto bytes = removals.join(QLatin1Char('\n')).toUtf8();
    if (!removals.isEmpty()) bytes.append('\n');
    writeFile(path, bytes);
}

bool makeDirectoryLink(const QString &link, const QString &target)
{
#ifdef Q_OS_WIN
    QProcess process;
    process.start(QStringLiteral("cmd.exe"),
                  {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                   QDir::toNativeSeparators(link), QDir::toNativeSeparators(target)});
    if (!process.waitForFinished(10000)) {
        process.kill();
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0
        && QFileInfo(link).isJunction();
#else
    Q_UNUSED(target)
    return QFile::link(target, link);
#endif
}

bool runPowerShell(const QString &script, QString &output, int &exitCode)
{
    const auto powershell = QStandardPaths::findExecutable(QStringLiteral("powershell"));
    if (powershell.isEmpty()) return false;
    const QByteArray utf16(reinterpret_cast<const char *>(script.utf16()), script.size() * 2);
    QProcess process;
    process.start(powershell, {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                               QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                               QStringLiteral("-EncodedCommand"), QString::fromLatin1(utf16.toBase64())});
    if (!process.waitForStarted(10000)) return false;
    if (!process.waitForFinished(120000)) {
        process.kill();
        process.waitForFinished(5000);
        return false;
    }
    output = QString::fromUtf8(process.readAllStandardOutput())
        + QString::fromLocal8Bit(process.readAllStandardError());
    exitCode = process.exitCode();
    return process.exitStatus() == QProcess::NormalExit;
}

} // namespace

class ResetPlanTests : public QObject {
    Q_OBJECT
private slots:
    void whitelistContents();
    void whitelistRejectsUnsafePaths();
    void planDeletesOnlyWhitelistedState();
    void planOrdersNetworksPeersMoonsPlanet();
    void planPreservesUnknownAndIdentityFiles();
    void planetOverrideRule();
    void planRejectsLinksAndUnsafeStateParents();
    void planRejectsUnsafeListings();
    void mergeRemovesOnlyStaleRelayKey();
    void mergeMissingAndEmptyLocalConfig();
    void mergeInvalidLocalConfig();
    void mergeNonObjectSettings();
    void scanReportsMissingDirectory();
    void scanEnumeratesStateDirectoriesOnly();
    void scanMarksDirectoryLinks();
    void windowsScriptContent();
    void windowsScriptContainsLocalConfigInstaller();
    void macScriptContent();
    void windowsDryRunCleanup();
    void windowsDryRunRejectsUnlistedPath();
    void windowsDryRunRejectsTraversal();
    void windowsDryRunLocalConfig();
    void windowsDryRunRejectsChangedLocalConfig();
};

void ResetPlanTests::whitelistContents()
{
    QCOMPARE(deletionWhitelist().size(), 5);
    QVERIFY(whitelistedDeletion(QStringLiteral("networks.d/0123456789abcdef.conf")));
    QVERIFY(whitelistedDeletion(QStringLiteral("networks.d/0123456789abcdef.local.conf")));
    QVERIFY(whitelistedDeletion(QStringLiteral("peers.d/0123456789.peer")));
    QVERIFY(whitelistedDeletion(QStringLiteral("moons.d/0123456789abcdef.moon")));
    QVERIFY(whitelistedDeletion(QStringLiteral("planet")));

    QVERIFY(!whitelistedDeletion(QStringLiteral("identity.secret")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("identity.public")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("authtoken.secret")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("metricstoken.secret")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("zerotier-one.port")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("local.conf")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("controller.d/db")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("networks.d/0123456789ABCDEF.conf")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("networks.d/0123456789abcdef.conf.bak")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("peers.d/0123456789.peer.bak")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("moons.d/0123456789abcdef.moon/x")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("Planet")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("planet.bak")));
}

void ResetPlanTests::whitelistRejectsUnsafePaths()
{
    QVERIFY(!whitelistedDeletion(QString()));
    QVERIFY(!whitelistedDeletion(QStringLiteral("../planet")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("..")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("/planet")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("C:/ProgramData/ZeroTier/One/planet")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("networks.d/../planet")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("networks.d//0123456789abcdef.conf")));
    QVERIFY(!whitelistedDeletion(QStringLiteral("networks.d\\0123456789abcdef.conf")));
}

void ResetPlanTests::planDeletesOnlyWhitelistedState()
{
    const auto snapshot = snapshotOf({
        fileEntry(QStringLiteral("networks.d/0123456789abcdef.conf")),
        fileEntry(QStringLiteral("networks.d/0123456789abcdef.local.conf")),
        fileEntry(QStringLiteral("peers.d/0123456789.peer")),
        fileEntry(QStringLiteral("moons.d/0123456789abcdef.moon")),
        fileEntry(QStringLiteral("planet")),
        fileEntry(QStringLiteral("identity.secret")),
        fileEntry(QStringLiteral("identity.public")),
        fileEntry(QStringLiteral("authtoken.secret")),
        fileEntry(QStringLiteral("metricstoken.secret")),
        fileEntry(QStringLiteral("zerotier-one.port")),
        fileEntry(QStringLiteral("local.conf"))
    });
    const auto plan = buildPlan(snapshot, bytesOf(QStringLiteral("{\"settings\":{}}")), true);
    QVERIFY2(plan.valid(), qPrintable(plan.error));
    QCOMPARE(plan.removals, QStringList({
        QStringLiteral("networks.d/0123456789abcdef.conf"),
        QStringLiteral("networks.d/0123456789abcdef.local.conf"),
        QStringLiteral("peers.d/0123456789.peer"),
        QStringLiteral("moons.d/0123456789abcdef.moon"),
        QStringLiteral("planet")
    }));
    for (const auto &kept : {QStringLiteral("identity.secret"), QStringLiteral("identity.public"),
                             QStringLiteral("authtoken.secret"), QStringLiteral("metricstoken.secret"),
                             QStringLiteral("zerotier-one.port"), QStringLiteral("local.conf")})
        QVERIFY(!plan.removals.contains(kept));
}

void ResetPlanTests::planOrdersNetworksPeersMoonsPlanet()
{
    const auto snapshot = snapshotOf({
        fileEntry(QStringLiteral("planet")),
        fileEntry(QStringLiteral("moons.d/bbbbbbbbbbbbbbbb.moon")),
        fileEntry(QStringLiteral("moons.d/aaaaaaaaaaaaaaaa.moon")),
        fileEntry(QStringLiteral("peers.d/bbbbbbbbbb.peer")),
        fileEntry(QStringLiteral("peers.d/aaaaaaaaaa.peer")),
        fileEntry(QStringLiteral("networks.d/bbbbbbbbbbbbbbbb.conf")),
        fileEntry(QStringLiteral("networks.d/aaaaaaaaaaaaaaaa.conf"))
    });
    const auto plan = buildPlan(snapshot, {}, false);
    QVERIFY2(plan.valid(), qPrintable(plan.error));
    QCOMPARE(plan.removals, QStringList({
        QStringLiteral("networks.d/aaaaaaaaaaaaaaaa.conf"),
        QStringLiteral("networks.d/bbbbbbbbbbbbbbbb.conf"),
        QStringLiteral("peers.d/aaaaaaaaaa.peer"),
        QStringLiteral("peers.d/bbbbbbbbbb.peer"),
        QStringLiteral("moons.d/aaaaaaaaaaaaaaaa.moon"),
        QStringLiteral("moons.d/bbbbbbbbbbbbbbbb.moon"),
        QStringLiteral("planet")
    }));
}

void ResetPlanTests::planPreservesUnknownAndIdentityFiles()
{
    const auto snapshot = snapshotOf({
        dirEntry(QStringLiteral("networks.d")),
        fileEntry(QStringLiteral("networks.d/0123456789abcdef.conf")),
        fileEntry(QStringLiteral("networks.d/notes.txt")),
        fileEntry(QStringLiteral("networks.d/0123456789abcdef.conf.backup")),
        dirEntry(QStringLiteral("peers.d")),
        fileEntry(QStringLiteral("peers.d/0123456789.peer")),
        fileEntry(QStringLiteral("peers.d/notes.txt")),
        dirEntry(QStringLiteral("moons.d")),
        fileEntry(QStringLiteral("moons.d/0123456789abcdef.moon")),
        fileEntry(QStringLiteral("moons.d/notes.txt")),
        dirEntry(QStringLiteral("controller.d")),
        fileEntry(QStringLiteral("controller.d/db")),
        fileEntry(QStringLiteral("planet.bak")),
        fileEntry(QStringLiteral("trustedpaths"))
    });
    const auto plan = buildPlan(snapshot, {}, false);
    QVERIFY2(plan.valid(), qPrintable(plan.error));
    QCOMPARE(plan.removals.size(), 3);
    QVERIFY(!plan.removals.contains(QStringLiteral("networks.d/notes.txt")));
    QVERIFY(!plan.removals.contains(QStringLiteral("networks.d/0123456789abcdef.conf.backup")));
    QVERIFY(!plan.removals.contains(QStringLiteral("peers.d/notes.txt")));
    QVERIFY(!plan.removals.contains(QStringLiteral("moons.d/notes.txt")));
    QVERIFY(!plan.removals.contains(QStringLiteral("controller.d/db")));
    QVERIFY(!plan.removals.contains(QStringLiteral("planet.bak")));
    QVERIFY(!plan.removals.contains(QStringLiteral("trustedpaths")));
}

void ResetPlanTests::planetOverrideRule()
{
    // A regular "planet" file is the custom root override and is deleted.
    {
        const auto plan = buildPlan(snapshotOf({fileEntry(QStringLiteral("planet"))}), {}, false);
        QVERIFY2(plan.valid(), qPrintable(plan.error));
        QCOMPARE(plan.removals, QStringList{QStringLiteral("planet")});
    }
    // A directory or unknown-cased name is not the file ZeroTier reads.
    {
        const auto plan = buildPlan(snapshotOf({
            dirEntry(QStringLiteral("planet")),
            fileEntry(QStringLiteral("Planet")),
            fileEntry(QStringLiteral("planet.bak"))
        }), {}, false);
        QVERIFY2(plan.valid(), qPrintable(plan.error));
        QVERIFY(plan.removals.isEmpty());
    }
    // Symlink must never be followed or deleted.
    {
        const auto plan = buildPlan(snapshotOf({linkEntry(QStringLiteral("planet"))}), {}, false);
        QVERIFY(!plan.valid());
        QVERIFY(plan.error.contains(QStringLiteral("planet")));
        QVERIFY(plan.removals.isEmpty());
    }
}

void ResetPlanTests::planRejectsLinksAndUnsafeStateParents()
{
    for (const auto &name : {QStringLiteral("networks.d"), QStringLiteral("peers.d"), QStringLiteral("moons.d")}) {
        const auto plan = buildPlan(snapshotOf({linkEntry(name)}), {}, false);
        QVERIFY2(!plan.valid(), qPrintable(name));
        QVERIFY(plan.removals.isEmpty());
    }
    {
        const auto plan = buildPlan(snapshotOf({
            fileEntry(QStringLiteral("networks.d")),
            fileEntry(QStringLiteral("networks.d/0123456789abcdef.conf"))
        }), {}, false);
        QVERIFY(!plan.valid());
    }
    {
        const auto plan = buildPlan(snapshotOf({
            dirEntry(QStringLiteral("networks.d")),
            linkEntry(QStringLiteral("networks.d/0123456789abcdef.conf"))
        }), {}, false);
        QVERIFY(!plan.valid());
        QVERIFY(plan.removals.isEmpty());
    }
    {
        // Unknown links are preserved, not followed.
        const auto plan = buildPlan(snapshotOf({
            dirEntry(QStringLiteral("networks.d")),
            linkEntry(QStringLiteral("networks.d/notes.link"))
        }), {}, false);
        QVERIFY2(plan.valid(), qPrintable(plan.error));
        QVERIFY(plan.removals.isEmpty());
    }
}

void ResetPlanTests::planRejectsUnsafeListings()
{
    for (const auto &bad : {QStringLiteral("../planet"), QStringLiteral("/planet"),
                            QStringLiteral("networks.d/../planet"),
                            QStringLiteral("C:/ProgramData/ZeroTier/One/planet"),
                            QStringLiteral("networks.d\\0123456789abcdef.conf")}) {
        const auto plan = buildPlan(snapshotOf({fileEntry(bad)}), {}, false);
        QVERIFY2(!plan.valid(), qPrintable(bad));
        QVERIFY(plan.removals.isEmpty());
    }
}

void ResetPlanTests::mergeRemovesOnlyStaleRelayKey()
{
    const auto original = bytesOf(QStringLiteral(R"({
        "settings": {
            "allowTcpFallbackRelay": false,
            "forceTcpRelay": true,
            "tcpFallbackRelay": "1.2.3.4/9443",
            "primaryPort": 19993,
            "unknownSetting": {"nested": [1, 2, 3]}
        },
        "physical": {"10.0.0.0/24": {"blacklist": true}},
        "virtual": {"a1b2c3d4e5": {"try": ["5.6.7.8/9993"]}},
        "customTopLevel": "keep"
    })"));
    const auto merge = mergeLocalConfig(original, true);
    QVERIFY2(merge.ok, qPrintable(merge.error));
    QVERIFY(merge.changed);
    QVERIFY(!merge.content.contains("tcpFallbackRelay"));
    QVERIFY(merge.content.contains("allowTcpFallbackRelay"));
    QVERIFY(merge.content.contains("forceTcpRelay"));
    QVERIFY(merge.content.contains("primaryPort"));
    QVERIFY(merge.content.contains("unknownSetting"));
    QVERIFY(merge.content.contains("physical"));
    QVERIFY(merge.content.contains("virtual"));
    QVERIFY(merge.content.contains("customTopLevel"));
    const auto root = QJsonDocument::fromJson(merge.content).object();
    QCOMPARE(root.value(QStringLiteral("settings")).toObject().value(QStringLiteral("allowTcpFallbackRelay")).toBool(), false);
    QCOMPARE(root.value(QStringLiteral("settings")).toObject().value(QStringLiteral("forceTcpRelay")).toBool(), true);
    QCOMPARE(root.value(QStringLiteral("settings")).toObject().value(QStringLiteral("primaryPort")).toInt(), 19993);
    QCOMPARE(root.value(QStringLiteral("customTopLevel")).toString(), QStringLiteral("keep"));
    QVERIFY(!root.value(QStringLiteral("settings")).toObject().contains(QStringLiteral("tcpFallbackRelay")));
}

void ResetPlanTests::mergeMissingAndEmptyLocalConfig()
{
    const auto missing = mergeLocalConfig({}, false);
    QVERIFY(missing.ok);
    QVERIFY(!missing.changed);
    QVERIFY(missing.content.isEmpty());

    const auto empty = mergeLocalConfig(QByteArray(), true);
    QVERIFY(empty.ok);
    QVERIFY(!empty.changed);

    const auto whitespace = mergeLocalConfig(QByteArray("  \r\n\t"), true);
    QVERIFY(whitespace.ok);
    QVERIFY(!whitespace.changed);

    const auto withoutRelay = mergeLocalConfig(bytesOf(QStringLiteral("{\"settings\":{\"forceTcpRelay\":true}}")), true);
    QVERIFY(withoutRelay.ok);
    QVERIFY(!withoutRelay.changed);
}

void ResetPlanTests::mergeInvalidLocalConfig()
{
    for (const auto &invalid : {QByteArray("{"), QByteArray("[]"), QByteArray("null"),
                                QByteArray("{\"settings\":\"x\"}"),
                                QByteArray("{\"settings\":[]}")}) {
        const auto merge = mergeLocalConfig(invalid, true);
        QVERIFY2(!merge.ok, invalid.constData());
        QVERIFY(merge.content == invalid);
        // The plan builder must reject before producing any delete list.
        const auto plan = buildPlan(snapshotOf({fileEntry(QStringLiteral("planet"))}), invalid, true);
        QVERIFY(!plan.valid());
        QVERIFY(plan.removals.isEmpty());
    }
}

void ResetPlanTests::mergeNonObjectSettings()
{
    const auto merge = mergeLocalConfig(bytesOf(QStringLiteral("{\"settings\":42}")), true);
    QVERIFY(!merge.ok);
    QVERIFY(merge.error.contains(QStringLiteral("settings")));
}

void ResetPlanTests::scanReportsMissingDirectory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto snapshot = scanDataDirectory(directory.filePath(QStringLiteral("missing")));
    QVERIFY(!snapshot.valid());
    QVERIFY(snapshot.error.contains(QStringLiteral("不存在")));
}

void ResetPlanTests::scanEnumeratesStateDirectoriesOnly()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto root = directory.filePath(QStringLiteral("One"));
    writeFile(root + QStringLiteral("/networks.d/0123456789abcdef.conf"));
    writeFile(root + QStringLiteral("/networks.d/0123456789abcdef.local.conf"));
    writeFile(root + QStringLiteral("/networks.d/notes.txt"));
    writeFile(root + QStringLiteral("/peers.d/0123456789.peer"));
    writeFile(root + QStringLiteral("/moons.d/0123456789abcdef.moon"));
    writeFile(root + QStringLiteral("/controller.d/db"));
    writeFile(root + QStringLiteral("/identity.secret"), QByteArray("secret"));
    writeFile(root + QStringLiteral("/planet"), QByteArray("planet"));

    const auto snapshot = scanDataDirectory(root);
    QVERIFY2(snapshot.valid(), qPrintable(snapshot.error));
    QStringList paths;
    for (const auto &entry : snapshot.entries) paths.append(entry.path);
    QVERIFY(paths.contains(QStringLiteral("networks.d/0123456789abcdef.conf")));
    QVERIFY(paths.contains(QStringLiteral("networks.d/0123456789abcdef.local.conf")));
    QVERIFY(paths.contains(QStringLiteral("networks.d/notes.txt")));
    QVERIFY(paths.contains(QStringLiteral("peers.d/0123456789.peer")));
    QVERIFY(paths.contains(QStringLiteral("moons.d/0123456789abcdef.moon")));
    QVERIFY(paths.contains(QStringLiteral("controller.d")));
    QVERIFY(!paths.contains(QStringLiteral("controller.d/db")));

    const auto plan = buildPlan(snapshot, {}, false);
    QVERIFY2(plan.valid(), qPrintable(plan.error));
    QCOMPARE(plan.removals.size(), 5);
}

void ResetPlanTests::scanMarksDirectoryLinks()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto root = directory.filePath(QStringLiteral("One"));
    const auto real = directory.filePath(QStringLiteral("real"));
    QDir().mkpath(root);
    QDir().mkpath(real + QStringLiteral("/nested"));
    writeFile(real + QStringLiteral("/nested/0123456789abcdef.moon"));
    if (!makeDirectoryLink(root + QStringLiteral("/moons.d"), real)) QSKIP("Directory links are not available");
    QVERIFY(QFileInfo(root + QStringLiteral("/moons.d")).isJunction()
            || QFileInfo(root + QStringLiteral("/moons.d")).isSymLink());

    const auto snapshot = scanDataDirectory(root);
    QVERIFY2(snapshot.valid(), qPrintable(snapshot.error));
    bool foundLink = false;
    bool foundChild = false;
    for (const auto &entry : snapshot.entries) {
        if (entry.path == QStringLiteral("moons.d")) {
            foundLink = entry.kind == EntryKind::Link;
        }
        if (entry.path.startsWith(QStringLiteral("moons.d/"))) foundChild = true;
    }
    QVERIFY(foundLink);
    QVERIFY(!foundChild);
    const auto plan = buildPlan(snapshot, {}, false);
    QVERIFY(!plan.valid());
}

void ResetPlanTests::windowsScriptContent()
{
    ScriptOptions options;
    options.dataRoot = QStringLiteral("C:/ProgramData/ZeroTier/One");
    options.removalListPath = QStringLiteral("C:/Temp/zto-reset/removals.txt");
    options.cliPath = QStringLiteral("C:/Program Files/ZeroTier/One/zerotier-one_x64.exe");
    const auto script = windowsResetScript(options);
    QVERIFY(!script.isEmpty());

    QVERIFY(script.contains(QStringLiteral("Stop-Service -Name 'ZeroTierOneService'")));
    QVERIFY(script.contains(QStringLiteral("Start-Service -Name 'ZeroTierOneService'")));
    QVERIFY(script.contains(QStringLiteral("stage=stop status=ok")));
    QVERIFY(script.contains(QStringLiteral("stage=cleanup status=ok")));
    QVERIFY(script.contains(QStringLiteral("stage=localconf status=ok")));
    QVERIFY(script.contains(QStringLiteral("stage=start status=ok")));
    QVERIFY(script.contains(QStringLiteral("stage=done status=ok")));
    QVERIFY(script.contains(QStringLiteral("Remove-Item -LiteralPath $target -Force")));
    QVERIFY(!script.contains(QStringLiteral("Remove-Item -Recurse")));
    QVERIFY(!script.contains(QStringLiteral("Remove-Item -LiteralPath $dataRoot")));
    QVERIFY(!script.contains(QStringLiteral("Get-ChildItem")));
    QVERIFY(!script.contains(QStringLiteral("*.conf")));
    QVERIFY(script.contains(QStringLiteral("C:/Temp/zto-reset/removals.txt")));
    QVERIFY(script.contains(QStringLiteral("Get-Content -LiteralPath $removalListPath")));
    QVERIFY(script.contains(QStringLiteral("$target.StartsWith($rootPrefix")));
    QVERIFY(script.contains(QStringLiteral("$allowedPatterns")));
    QVERIFY(script.contains(QStringLiteral("networks\\.d/[0-9a-f]{16}\\.conf")));
    QVERIFY(script.contains(QStringLiteral("peers\\.d/[0-9a-f]{10}\\.peer")));
    QVERIFY(script.contains(QStringLiteral("moons\\.d/[0-9a-f]{16}\\.moon")));
    QVERIFY(script.contains(QStringLiteral("^planet$")));
    QVERIFY(script.contains(QStringLiteral("zerotier-one.port")));
    QVERIFY(script.contains(QStringLiteral("/status")));
    QVERIFY(script.contains(QStringLiteral("authtoken.secret")));
    QVERIFY(script.contains(QStringLiteral("stage=recovery status=ok")));
    // The removal list is an external file, never a command-line argument, so
    // thousands of peers cannot exceed the Windows command-line limit.
    QVERIFY(!script.contains(QStringLiteral("foreach ($relative in @(")));

    const int stopOk = script.indexOf(QStringLiteral("stage=stop status=ok"));
    const int remove = script.indexOf(QStringLiteral("Remove-Item -LiteralPath $target"));
    const int startOk = script.indexOf(QStringLiteral("stage=start status=ok"));
    const int stopThrow = script.indexOf(QStringLiteral("if ($service.Status -ne 'Stopped') { throw"));
    QVERIFY(stopOk >= 0);
    QVERIFY(remove > stopOk);
    QVERIFY(startOk > remove);
    QVERIFY(stopThrow >= 0 && stopThrow < remove);
    // The delete loop lives inside the try block; the catch (which recovers the
    // service) is after it. A stop failure jumps straight to the catch.
    QVERIFY(script.indexOf(QStringLiteral("} catch {")) > remove);
}

void ResetPlanTests::windowsScriptContainsLocalConfigInstaller()
{
    ScriptOptions options;
    options.dataRoot = QStringLiteral("C:/ProgramData/ZeroTier/One");
    options.removalListPath = QStringLiteral("C:/Temp/zto-reset/removals.txt");
    options.localConfigChanged = true;
    options.localConfigPath = QStringLiteral("C:/ProgramData/ZeroTier/One/local.conf");
    options.proposedLocalConfigPath = QStringLiteral("C:/Temp/zto-reset/local.conf");
    options.expectedLocalConfigSha256 = QString(64, QLatin1Char('a'));
    const auto script = windowsResetScript(options);
    QVERIFY(script.contains(QStringLiteral("local.conf changed since it was validated")));
    QVERIFY(script.contains(QStringLiteral("local.conf is not a regular file")));
    QVERIFY(script.contains(QStringLiteral("C:/Temp/zto-reset/local.conf")));
    QVERIFY(script.contains(QStringLiteral("stage=localconf status=ok")));
    QVERIFY(!script.contains(QStringLiteral("tcpFallbackRelay")));
}

void ResetPlanTests::macScriptContent()
{
    ScriptOptions options;
    options.dataRoot = QStringLiteral("/Library/Application Support/ZeroTier/One");
    options.removalListPath = QStringLiteral("/tmp/zto-reset/removals.txt");
    options.cliPath = QStringLiteral("/usr/local/bin/zerotier-cli");
    const auto script = macResetScript(options);
    QVERIFY(!script.isEmpty());
    QVERIFY(script.contains(QStringLiteral("set -eu")));
    QVERIFY(script.contains(QStringLiteral("rm -f -- \"$target\"")));
    QVERIFY(!script.contains(QStringLiteral("rm -rf")));
    QVERIFY(script.contains(QStringLiteral("done < \"$removalListPath\"")));
    QVERIFY(script.contains(QStringLiteral("/tmp/zto-reset/removals.txt")));
    QVERIFY(script.contains(QStringLiteral("fail 'not-installed'")));
    QVERIFY(script.contains(QStringLiteral("launchctl print system/com.zerotier.one")));
    QVERIFY(script.contains(QStringLiteral("zerotier-cli")));
    QVERIFY(!script.contains(QStringLiteral("ZTO_RESET_LIST")));
    const int stopOk = script.indexOf(QStringLiteral("marker stop ok"));
    const int remove = script.indexOf(QStringLiteral("rm -f -- \"$target\""));
    QVERIFY(stopOk >= 0 && remove > stopOk);
}

void ResetPlanTests::windowsDryRunCleanup()
{
    if (QStandardPaths::findExecutable(QStringLiteral("powershell")).isEmpty())
        QSKIP("PowerShell is not available");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto root = directory.filePath(QStringLiteral("One"));
    writeFile(root + QStringLiteral("/networks.d/0123456789abcdef.conf"), QByteArray("net"));
    writeFile(root + QStringLiteral("/networks.d/0123456789abcdef.local.conf"), QByteArray("local"));
    writeFile(root + QStringLiteral("/networks.d/notes.txt"), QByteArray("keep"));
    writeFile(root + QStringLiteral("/peers.d/0123456789.peer"), QByteArray("peer"));
    writeFile(root + QStringLiteral("/peers.d/notes.txt"), QByteArray("keep"));
    writeFile(root + QStringLiteral("/moons.d/0123456789abcdef.moon"), QByteArray("moon"));
    writeFile(root + QStringLiteral("/planet"), QByteArray("planet"));
    writeFile(root + QStringLiteral("/identity.secret"), QByteArray("secret"));
    writeFile(root + QStringLiteral("/identity.public"), QByteArray("public"));
    writeFile(root + QStringLiteral("/authtoken.secret"), QByteArray("token"));
    writeFile(root + QStringLiteral("/zerotier-one.port"), QByteArray("9993"));
    writeFile(root + QStringLiteral("/local.conf"), QByteArray("{}"));
    writeFile(root + QStringLiteral("/controller.d/db"), QByteArray("db"));
    writeFile(root + QStringLiteral("/unknown.bin"), QByteArray("unknown"));

    ScriptOptions options;
    options.dataRoot = root;
    const QStringList removals = {QStringLiteral("networks.d/0123456789abcdef.conf"),
                                  QStringLiteral("networks.d/0123456789abcdef.local.conf"),
                                  QStringLiteral("peers.d/0123456789.peer"),
                                  QStringLiteral("moons.d/0123456789abcdef.moon"),
                                  QStringLiteral("planet")};
    writeRemovalList(directory.filePath(QStringLiteral("removals.txt")), removals);
    options.removalListPath = directory.filePath(QStringLiteral("removals.txt"));
    options.manageService = false;
    QString output;
    int exitCode = -1;
    QVERIFY(runPowerShell(windowsResetScript(options), output, exitCode));
    QVERIFY2(exitCode == 0, qPrintable(output));
    QVERIFY(output.contains(QStringLiteral("stage=cleanup status=ok")));
    QVERIFY(output.contains(QStringLiteral("stage=done status=ok")));
    for (const auto &removed : removals)
        QVERIFY2(!QFileInfo::exists(root + QLatin1Char('/') + removed), qPrintable(removed));
    for (const auto &kept : {QStringLiteral("networks.d/notes.txt"), QStringLiteral("peers.d/notes.txt"),
                             QStringLiteral("identity.secret"), QStringLiteral("identity.public"),
                             QStringLiteral("authtoken.secret"), QStringLiteral("zerotier-one.port"),
                             QStringLiteral("local.conf"), QStringLiteral("controller.d/db"),
                             QStringLiteral("unknown.bin")})
        QVERIFY2(QFileInfo::exists(root + QLatin1Char('/') + kept), qPrintable(kept));
    QCOMPARE(readFile(root + QStringLiteral("/identity.secret")), QByteArray("secret"));
    QCOMPARE(readFile(root + QStringLiteral("/authtoken.secret")), QByteArray("token"));
}

void ResetPlanTests::windowsDryRunRejectsUnlistedPath()
{
    if (QStandardPaths::findExecutable(QStringLiteral("powershell")).isEmpty())
        QSKIP("PowerShell is not available");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto root = directory.filePath(QStringLiteral("One"));
    writeFile(root + QStringLiteral("/identity.secret"), QByteArray("secret"));
    writeFile(root + QStringLiteral("/networks.d/0123456789abcdef.conf"), QByteArray("net"));

    ScriptOptions options;
    options.dataRoot = root;
    writeRemovalList(directory.filePath(QStringLiteral("removals.txt")),
                     {QStringLiteral("identity.secret")});
    options.removalListPath = directory.filePath(QStringLiteral("removals.txt"));
    options.manageService = false;
    QString output;
    int exitCode = -1;
    QVERIFY(runPowerShell(windowsResetScript(options), output, exitCode));
    QVERIFY2(exitCode != 0, qPrintable(output));
    QVERIFY(output.contains(QStringLiteral("not whitelisted")));
    QVERIFY(QFileInfo::exists(root + QStringLiteral("/identity.secret")));
    QVERIFY(QFileInfo::exists(root + QStringLiteral("/networks.d/0123456789abcdef.conf")));
}

void ResetPlanTests::windowsDryRunRejectsTraversal()
{
    if (QStandardPaths::findExecutable(QStringLiteral("powershell")).isEmpty())
        QSKIP("PowerShell is not available");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto root = directory.filePath(QStringLiteral("One"));
    writeFile(root + QStringLiteral("/networks.d/0123456789abcdef.conf"), QByteArray("net"));
    writeFile(directory.filePath(QStringLiteral("outside.txt")), QByteArray("outside"));

    ScriptOptions options;
    options.dataRoot = root;
    writeRemovalList(directory.filePath(QStringLiteral("removals.txt")),
                     {QStringLiteral("../outside.txt"),
                      QStringLiteral("networks.d/0123456789abcdef.conf")});
    options.removalListPath = directory.filePath(QStringLiteral("removals.txt"));
    options.manageService = false;
    QString output;
    int exitCode = -1;
    QVERIFY(runPowerShell(windowsResetScript(options), output, exitCode));
    QVERIFY2(exitCode != 0, qPrintable(output));
    QVERIFY(output.contains(QStringLiteral("not whitelisted")));
    QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("outside.txt"))));
    QVERIFY(QFileInfo::exists(root + QStringLiteral("/networks.d/0123456789abcdef.conf")));
}

void ResetPlanTests::windowsDryRunLocalConfig()
{
    if (QStandardPaths::findExecutable(QStringLiteral("powershell")).isEmpty())
        QSKIP("PowerShell is not available");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto root = directory.filePath(QStringLiteral("One"));
    const QByteArray original = QByteArray(R"({"settings":{"allowTcpFallbackRelay":true,"forceTcpRelay":false,"tcpFallbackRelay":"1.2.3.4/9443","primaryPort":19993},"custom":{"keep":1}})");
    writeFile(root + QStringLiteral("/local.conf"), original);
    writeFile(root + QStringLiteral("/networks.d/0123456789abcdef.conf"), QByteArray("net"));
    const auto merge = mergeLocalConfig(original, true);
    QVERIFY2(merge.ok, qPrintable(merge.error));
    QVERIFY(merge.changed);
    const auto proposed = directory.filePath(QStringLiteral("proposed.conf"));
    writeFile(proposed, merge.content);

    ScriptOptions options;
    options.dataRoot = root;
    writeRemovalList(directory.filePath(QStringLiteral("removals.txt")),
                     {QStringLiteral("networks.d/0123456789abcdef.conf")});
    options.removalListPath = directory.filePath(QStringLiteral("removals.txt"));
    options.manageService = false;
    options.localConfigChanged = true;
    options.localConfigPath = root + QStringLiteral("/local.conf");
    options.proposedLocalConfigPath = proposed;
    options.expectedLocalConfigSha256 = sha256Hex(original);
    QString output;
    int exitCode = -1;
    QVERIFY(runPowerShell(windowsResetScript(options), output, exitCode));
    QVERIFY2(exitCode == 0, qPrintable(output));
    QVERIFY(output.contains(QStringLiteral("stage=localconf status=ok")));
    const auto updated = QJsonDocument::fromJson(readFile(root + QStringLiteral("/local.conf"))).object();
    QVERIFY(!updated.value(QStringLiteral("settings")).toObject().contains(QStringLiteral("tcpFallbackRelay")));
    QCOMPARE(updated.value(QStringLiteral("settings")).toObject().value(QStringLiteral("primaryPort")).toInt(), 19993);
    QCOMPARE(updated.value(QStringLiteral("custom")).toObject().value(QStringLiteral("keep")).toInt(), 1);
}

void ResetPlanTests::windowsDryRunRejectsChangedLocalConfig()
{
    if (QStandardPaths::findExecutable(QStringLiteral("powershell")).isEmpty())
        QSKIP("PowerShell is not available");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto root = directory.filePath(QStringLiteral("One"));
    const QByteArray original = QByteArray(R"({"settings":{"tcpFallbackRelay":"1.2.3.4/9443"}})");
    writeFile(root + QStringLiteral("/local.conf"), original);
    const auto proposed = directory.filePath(QStringLiteral("proposed.conf"));
    writeFile(proposed, QByteArray(R"({"settings":{}})"));

    ScriptOptions options;
    options.dataRoot = root;
    writeRemovalList(directory.filePath(QStringLiteral("removals.txt")), {});
    options.removalListPath = directory.filePath(QStringLiteral("removals.txt"));
    options.manageService = false;
    options.localConfigChanged = true;
    options.localConfigPath = root + QStringLiteral("/local.conf");
    options.proposedLocalConfigPath = proposed;
    options.expectedLocalConfigSha256 = QString(64, QLatin1Char('0'));
    QString output;
    int exitCode = -1;
    QVERIFY(runPowerShell(windowsResetScript(options), output, exitCode));
    QVERIFY2(exitCode != 0, qPrintable(output));
    QVERIFY(output.contains(QStringLiteral("stage=localconf status=failed")));
    QCOMPARE(readFile(root + QStringLiteral("/local.conf")), original);
}

QTEST_GUILESS_MAIN(ResetPlanTests)
#include "ResetPlanTests.moc"
