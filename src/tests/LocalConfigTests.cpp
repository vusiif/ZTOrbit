#include "LocalConfig.h"
#include "NetworkBundle.h"
#include "ZeroTierClient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QtTest>

using namespace vusiif::zto::zerotier;
using namespace vusiif::zto::zerotier::localconfig;

namespace {
QString sha256(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeAll(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    return file.write(bytes) == bytes.size();
}

bool runPowerShell(const QString &script, int &exitCode, QString &output)
{
    const auto invocation = preparePowerShell(script);
    if (!invocation.directory || !invocation.directory->isValid()) return false;
    QProcess process;
    process.start("powershell.exe", invocation.arguments);
    if (!process.waitForFinished(20000)) return false;
    exitCode = process.exitCode();
    QFile log(invocation.logPath);
    if (log.open(QIODevice::ReadOnly)) output = QString::fromUtf8(log.readAll());
    return process.exitStatus() == QProcess::NormalExit;
}
}

class LocalConfigTests : public QObject {
    Q_OBJECT
    QTemporaryDir m_directory;
    QByteArray m_oldData;
    bool m_hadData = false;
private slots:
    void initTestCase()
    {
        QVERIFY(m_directory.isValid());
        m_hadData = qEnvironmentVariableIsSet("ZT_TEST_DATA_DIRECTORY");
        m_oldData = qgetenv("ZT_TEST_DATA_DIRECTORY");
        qputenv("ZT_TEST_DATA_DIRECTORY", m_directory.filePath("ZeroTier").toLocal8Bit());
        QVERIFY(QDir().mkpath(systemHomeDirectory()));
    }
    void cleanupTestCase()
    {
        if (m_hadData) qputenv("ZT_TEST_DATA_DIRECTORY", m_oldData);
        else qunsetenv("ZT_TEST_DATA_DIRECTORY");
    }
    void cleanup()
    {
        QFile::remove(localConfigPath());
    }
    void defaultCreationDoesNotOverwriteExistingFile()
    {
        const auto path = localConfigPath();
        QCOMPARE(path, QDir(systemHomeDirectory()).filePath("local.conf"));
#ifdef Q_OS_WIN
        QVERIFY(path.contains("/ZeroTier/One/"));
#elif defined(Q_OS_MACOS)
        QVERIFY(path.contains("/ZeroTier/One/"));
#endif
        QFile::remove(path);
        auto missing = read(path);
        QVERIFY(missing.ok);
        QVERIFY(!missing.state.exists);
        QVERIFY(missing.state.allowTcpFallbackRelay);
        QVERIFY(!missing.state.forceTcpRelay);
        QCOMPARE(missing.state.tcpFallbackRelay, QString());
        QVERIFY(missing.state.valid());

        const auto created = createDefault(path);
        QVERIFY2(created.ok, qPrintable(created.error));
        const auto expected = defaultContent();
        QCOMPARE(expected, QByteArray(
            "{\n"
            "    \"settings\": {\n"
            "        \"allowTcpFallbackRelay\": true,\n"
            "        \"forceTcpRelay\": false\n"
            "    }\n"
            "}\n"));
        QCOMPARE(readAll(path), expected);
        const auto parsed = parse(expected, true);
        QVERIFY(parsed.valid());
        QVERIFY(parsed.allowTcpFallbackRelay);
        QVERIFY(!parsed.forceTcpRelay);
        QVERIFY(parsed.tcpFallbackRelay.isEmpty());
        QVERIFY(!QJsonDocument::fromJson(expected).object().value("settings").toObject()
                     .contains("tcpFallbackRelay"));

        const QByteArray existing("{\n  \"settings\": {\"primaryPort\": 19999, \"allowTcpFallbackRelay\": false},\n"
                                 "  \"unknown\": {\"keep\": [1, 2, 3]}\n}\n");
        QVERIFY(writeAll(path, existing));
        const auto loaded = read(path);
        QVERIFY2(loaded.ok, qPrintable(loaded.error));
        QVERIFY(loaded.state.exists);
        QVERIFY(!loaded.state.allowTcpFallbackRelay);
        QVERIFY(!loaded.state.forceTcpRelay);
        QCOMPARE(loaded.state.root.value("settings").toObject().value("primaryPort").toInt(), 19999);
        // Reading never projects defaults back into the file.
        QCOMPARE(readAll(path), existing);
    }
    void invalidJsonAndShapesAreReportedAndPreserved()
    {
        const auto path = localConfigPath();
        const QList<QByteArray> invalid = {
            "not json",
            "[]",
            "{\"settings\": []}",
            "{\"settings\": {\"allowTcpFallbackRelay\": \"yes\"}}",
            "{\"settings\": {\"forceTcpRelay\": 1}}",
            "{\"settings\": {\"tcpFallbackRelay\": 42}}"
        };
        for (const auto &bytes : invalid) {
            QVERIFY(writeAll(path, bytes));
            const auto result = read(path);
            QVERIFY(!result.ok);
            QVERIFY(result.state.exists);
            QVERIFY(!result.state.error.isEmpty());
            QCOMPARE(readAll(path), bytes);
        }
        QFile::remove(path);
        QVERIFY(writeAll(path, invalid.first()));
        const auto ensure = ensureScript(path, m_directory.filePath("snapshot.conf"));
        QVERIFY(!ensure.isEmpty());
#ifdef Q_OS_WIN
        int code = 0;
        QString output;
        QVERIFY(runPowerShell(ensure, code, output));
        QVERIFY2(code == 0, qPrintable(output));
        QCOMPARE(readAll(path), invalid.first());
        QCOMPARE(readAll(m_directory.filePath("snapshot.conf")), invalid.first());
#endif
    }
    void mergeKeepsUnknownFieldsAndPrimaryPort()
    {
        const auto path = localConfigPath();
        const QByteArray original(
            "{\n"
            "  \"settings\": {\n"
            "    \"primaryPort\": 19999,\n"
            "    \"allowTcpFallbackRelay\": true,\n"
            "    \"forceTcpRelay\": false,\n"
            "    \"tcpFallbackRelay\": \"198.51.100.9/9443\",\n"
            "    \"extraSetting\": \"keep\"\n"
            "  },\n"
            "  \"physicalPath\": \"/var/lib/zerotier-one\",\n"
            "  \"unknown\": {\"nested\": [true, 42, \"x\"]}\n"
            "}\n");
        QVERIFY(writeAll(path, original));
        const auto state = read(path).state;
        QVERIFY(state.valid());
        QByteArray merged;
        QString error;
        QVERIFY2(mergeSwitches(state, false, false, merged, error), qPrintable(error));
        const auto switched = parse(merged, true);
        QVERIFY(switched.valid());
        QVERIFY(!switched.allowTcpFallbackRelay);
        QVERIFY(!switched.forceTcpRelay);
        const auto switchedSettings = switched.root.value("settings").toObject();
        QCOMPARE(switchedSettings.value("primaryPort").toInt(), 19999);
        QCOMPARE(switchedSettings.value("extraSetting").toString(), QString("keep"));
        QCOMPARE(switchedSettings.value("tcpFallbackRelay").toString(), QString("198.51.100.9/9443"));
        QCOMPARE(switched.root.value("physicalPath").toString(), QString("/var/lib/zerotier-one"));
        QCOMPARE(switched.root.value("unknown").toObject().value("nested").toArray().size(), 3);

        QVERIFY2(mergeRelay(switched, "203.0.113.7", 9443, merged, error), qPrintable(error));
        const auto relayed = parse(merged, true);
        QVERIFY(relayed.valid());
        QCOMPARE(relayed.tcpFallbackRelay, QString("203.0.113.7/9443"));
        QCOMPARE(relayed.relayIp, QString("203.0.113.7"));
        QCOMPARE(relayed.relayPort, 9443);
        QVERIFY(!relayed.allowTcpFallbackRelay);
        QVERIFY(!relayed.forceTcpRelay);
        QCOMPARE(relayed.root.value("settings").toObject().value("primaryPort").toInt(), 19999);

        State missing;
        QVERIFY2(mergeRelay(missing, "203.0.113.8", 9443, merged, error), qPrintable(error));
        const auto created = parse(merged, true);
        QCOMPARE(created.tcpFallbackRelay, QString("203.0.113.8/9443"));
        QVERIFY(created.allowTcpFallbackRelay);
        QVERIFY(!created.forceTcpRelay);
    }
    void relayFormatsAreStrict()
    {
        QString ip;
        int port = 0;
        QVERIFY(parseRelay("1.2.3.4/9443", ip, port));
        QCOMPARE(ip, QString("1.2.3.4"));
        QCOMPARE(port, 9443);
        QCOMPARE(nativeRelay(ip, port), QString("1.2.3.4/9443"));
        QVERIFY(parseDisplayRelay("1.2.3.4:9443", ip, port));
        QCOMPARE(displayRelay(ip, port), QString("1.2.3.4:9443"));
        for (const auto &value : {"", "1.2.3.4", "1.2.3.4/", "/9443", "1.2.3.4/0", "1.2.3.4/65536",
                                  "1.2.3.4/9443/1", "999.1.1.1/9443", "::1/9443"})
            QVERIFY2(!parseRelay(value, ip, port), value);
        for (const auto &value : {"", "1.2.3.4", "1.2.3.4:", ":9443", "1.2.3.4:9993:1", "::1:9443"})
            QVERIFY2(!parseDisplayRelay(value, ip, port), value);
        QVERIFY(parseDisplayRelay("1.2.3.4:9993", ip, port));
        QCOMPARE(port, 9993);

        State state;
        QByteArray content;
        QString error;
        QVERIFY(!mergeRelayDisplay(state, "1.2.3.4:9993", content, error));
        QVERIFY(!error.isEmpty());
        QVERIFY(mergeRelayDisplay(state, "1.2.3.4:9443", content, error));
        QCOMPARE(parse(content, true).tcpFallbackRelay, QString("1.2.3.4/9443"));
    }
    void candidatesFollowDocumentedMoonIdRules()
    {
        const QString networkId = "0123456789abcdef";
        const QJsonArray networks{
            QJsonObject{{"nwid", networkId}, {"name", "团队 网络"}},
            QJsonObject{{"nwid", "fedcba9876543210"}, {"name", "没有 Moon"}},
            QJsonObject{{"nwid", networkId}, {"name", "duplicate"}},
            QJsonObject{{"nwid", "not-a-network"}}
        };
        const QJsonDocument moons(QJsonArray{
            QJsonObject{{"id", "0000000123456789"},
                {"roots", QJsonArray{
                    QJsonObject{{"identity", "ffffffffff:0:other"}, {"stableEndpoints", QJsonArray{"203.0.113.99/9993"}}},
                    QJsonObject{{"identity", "0123456789:0:server"}, {"stableEndpoints",
                        QJsonArray{"192.0.2.10/9993", "192.0.2.11/9993"}}}}}},
            QJsonObject{{"id", "000000fedcba9876"},
                {"roots", QJsonArray{QJsonObject{{"identity", "fedcba9876:0:server"},
                    {"stableEndpoints", QJsonArray{"198.51.100.4/9993"}}}}}}
        });
        State state;
        state.relayIp = "192.0.2.10";
        state.relayPort = 9443;
        state.tcpFallbackRelay = "192.0.2.10/9443";
        const auto eligible = [](const QString &id) { return id != "fedcba9876543210"; };
        const auto list = candidates(networks, moons, eligible, state);
        QCOMPARE(list.size(), 1);
        const auto &candidate = list.first();
        QCOMPARE(candidate.networkId, networkId);
        QCOMPARE(candidate.name, QString("团队 网络"));
        QVERIFY(candidate.enabled);
        QCOMPARE(candidate.ip, QString("192.0.2.10"));
        QCOMPARE(candidate.relay, QString("192.0.2.10:9443"));
        QVERIFY(candidate.current);
        QVERIFY(candidate.note.contains(QStringLiteral("2")));

        const auto all = candidates(networks, moons, [](const QString &) { return true; }, State{});
        QCOMPARE(all.size(), 2);
        QVERIFY(all.at(1).enabled);
        QCOMPARE(all.at(1).relay, QString("198.51.100.4:9443"));
        QVERIFY(!all.at(1).current);

        const auto noMoon = candidates(networks, QJsonDocument(QJsonArray{}), [](const QString &) { return true; }, State{});
        QCOMPARE(noMoon.size(), 2);
        QVERIFY(!noMoon.first().enabled);
        QVERIFY(noMoon.first().reason.contains(QStringLiteral("Moon")));

        const QJsonDocument wrongRoot(QJsonArray{QJsonObject{{"id", "0000000123456789"},
            {"roots", QJsonArray{QJsonObject{{"identity", "ffffffffff:0:other"},
                {"stableEndpoints", QJsonArray{"192.0.2.10/9993"}}}}}}});
        const auto wrong = candidates({QJsonObject{{"nwid", networkId}}}, wrongRoot,
                                      [](const QString &) { return true; }, State{});
        QVERIFY(!wrong.first().enabled);
        QVERIFY(wrong.first().reason.contains(QStringLiteral("根节点")));

        const QJsonDocument badEndpoints(QJsonArray{QJsonObject{{"id", "0000000123456789"},
            {"roots", QJsonArray{QJsonObject{{"identity", "0123456789:0:server"},
                {"stableEndpoints", QJsonArray{"not-an-ip/9993", "::1/9993"}}}}}}});
        const auto bad = candidates({QJsonObject{{"nwid", networkId}}}, badEndpoints,
                                    [](const QString &) { return true; }, State{});
        QVERIFY(!bad.first().enabled);
        QVERIFY(!bad.first().reason.isEmpty());
        QVERIFY(candidates({}, moons, [](const QString &) { return true; }, State{}).isEmpty());
    }
    void exportRelayComparesParsedAddressAndPort()
    {
        const QString networkId = "0123456789abcdef";
        const QJsonDocument moons(QJsonArray{QJsonObject{{"id", "0000000123456789"},
            {"roots", QJsonArray{QJsonObject{{"identity", "0123456789:0:server"},
                {"stableEndpoints", QJsonArray{"192.0.2.10/9993"}}}}}}});
        auto state = parse("{\"settings\":{\"tcpFallbackRelay\":\"192.0.2.10/9443\"}}", true);
        QCOMPARE(exportRelay(state, networkId, moons), QString("192.0.2.10:9443"));
        state = parse("{\"settings\":{\"tcpFallbackRelay\":\"192.0.2.1/9443\"}}", true);
        QVERIFY(exportRelay(state, networkId, moons).isEmpty());
        state = parse("{\"settings\":{\"tcpFallbackRelay\":\"192.0.2.10/9993\"}}", true);
        QVERIFY(exportRelay(state, networkId, moons).isEmpty());
        state = parse("{\"settings\":{}}", true);
        QVERIFY(exportRelay(state, networkId, moons).isEmpty());
        QVERIFY(exportRelay(parse("{}", true), "fedcba9876543210", moons).isEmpty());
    }
    void bundleCarriesOnlyMatchingOptionalRelay()
    {
        bundle::Configuration configuration{"0123456789abcdef", "0000000123456789.moon",
                                           QByteArray("signed-moon"), "192.0.2.10:9443"};
        const auto encoded = bundle::encode(configuration);
        QVERIFY(!encoded.isEmpty());
        QCOMPARE(QJsonDocument::fromJson(encoded).object().value("tcpFallbackRelay").toString(),
                 QString("192.0.2.10:9443"));
        bundle::Configuration decoded;
        QString error;
        QVERIFY2(bundle::decode(encoded, decoded, error), qPrintable(error));
        QCOMPARE(decoded.tcpFallbackRelay, QString("192.0.2.10:9443"));

        configuration.tcpFallbackRelay.clear();
        const auto without = bundle::encode(configuration);
        QVERIFY(!without.isEmpty());
        QVERIFY(!QJsonDocument::fromJson(without).object().contains("tcpFallbackRelay"));
        QVERIFY(bundle::decode(without, decoded, error));
        QVERIFY(decoded.tcpFallbackRelay.isEmpty());

        const auto original = QJsonDocument::fromJson(encoded).object();
        const QList<QJsonValue> invalid{
            QJsonValue(), QJsonValue(1), QJsonValue(""), QJsonValue("not-an-ip:9443"),
            QJsonValue("192.0.2.10:9993"), QJsonValue("192.0.2.10"), QJsonValue("192.0.2.10:9443:1")};
        for (const auto &value : invalid) {
            auto object = original;
            if (value.isUndefined()) object.remove("tcpFallbackRelay");
            else object.insert("tcpFallbackRelay", value);
            if (value.isUndefined()) {
                QVERIFY(bundle::decode(QJsonDocument(object).toJson(), decoded, error));
                QVERIFY(decoded.tcpFallbackRelay.isEmpty());
            } else {
                QVERIFY(!bundle::decode(QJsonDocument(object).toJson(), decoded, error));
                QVERIFY(!error.isEmpty());
            }
        }
        configuration.tcpFallbackRelay = "192.0.2.10:9993";
        QVERIFY(bundle::encode(configuration).isEmpty());
    }
#ifdef Q_OS_WIN
    void windowsScriptsCreateAndInstallAtomically()
    {
        const auto directory = QDir(m_directory.filePath(QString::fromUtf8("带 空格")));
        QVERIFY(QDir().mkpath(directory.path()));
        const auto path = directory.filePath("local.conf");
        const auto snapshot = directory.filePath("snapshot.conf");
        QFile::remove(path);
        QFile::remove(snapshot);
        int code = 0;
        QString output;
        QVERIFY(runPowerShell(ensureScript(path, snapshot), code, output));
        QVERIFY2(code == 0, qPrintable(output));
        QCOMPARE(readAll(path), defaultContent());
        QCOMPARE(readAll(snapshot), defaultContent());

        const auto existing = QByteArray("{\"settings\":{\"allowTcpFallbackRelay\":false}}");
        QVERIFY(writeAll(path, existing));
        QVERIFY(runPowerShell(ensureScript(path, snapshot), code, output));
        QCOMPARE(code, 0);
        QCOMPARE(readAll(path), existing);
        QCOMPARE(readAll(snapshot), existing);

        const auto proposed = directory.filePath("proposed.conf");
        const auto proposedBytes = QByteArray("{\"settings\":{\"forceTcpRelay\":true}}");
        QVERIFY(writeAll(proposed, proposedBytes));
        QVERIFY(runPowerShell(installScript({path, proposed, sha256(existing)}), code, output));
        QVERIFY2(code == 0, qPrintable(output));
        QCOMPARE(readAll(path), proposedBytes);

        const auto changed = QByteArray("{\"settings\":{\"allowTcpFallbackRelay\":true}}");
        QVERIFY(writeAll(path, changed));
        QVERIFY(runPowerShell(installScript({path, proposed, sha256(existing)}), code, output));
        QVERIFY(code != 0);
        QCOMPARE(readAll(path), changed);

        QFile::remove(path);
        QVERIFY(runPowerShell(installScript({path, proposed, QString()}), code, output));
        QVERIFY2(code == 0, qPrintable(output));
        QCOMPARE(readAll(path), proposedBytes);
    }
#endif
};

QTEST_GUILESS_MAIN(LocalConfigTests)
#include "LocalConfigTests.moc"
