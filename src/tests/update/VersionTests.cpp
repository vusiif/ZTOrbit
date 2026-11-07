#include "UpdateVersion.h"

#include <QtTest>

using namespace vusiif::zto::update;

class VersionTests : public QObject {
    Q_OBJECT
private slots:
    void parsesValidVersions();
    void rejectsInvalidVersions();
    void comparesNumericComponents();
    void comparesMultiDigitPatch();
    void comparesPrereleaseOrdering();
    void treatsBuildMetadataAsEqual();
    void versionIsNewerHelper();
    void roundTripsToString();
};

void VersionTests::parsesValidVersions()
{
    const auto parsed = parseVersion(QStringLiteral("0.5.10-rc.1+build.7"));
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->major, 0);
    QCOMPARE(parsed->minor, 5);
    QCOMPARE(parsed->patch, 10);
    QCOMPARE(parsed->prerelease.size(), std::size_t(2));
    QCOMPARE(parsed->prerelease[0].kind, VersionIdentifier::Kind::Alphanumeric);
    QCOMPARE(parsed->prerelease[0].text, QStringLiteral("rc"));
    QCOMPARE(parsed->prerelease[1].kind, VersionIdentifier::Kind::Numeric);
    QCOMPARE(parsed->prerelease[1].number, quint64(1));
    QCOMPARE(parsed->buildMetadata, QStringLiteral("build.7"));

    const auto simple = parseVersion(QStringLiteral("12.34.56"));
    QVERIFY(simple.has_value());
    QCOMPARE(simple->major, 12);
    QCOMPARE(simple->minor, 34);
    QCOMPARE(simple->patch, 56);
    QVERIFY(!simple->isPrerelease());
}

void VersionTests::rejectsInvalidVersions()
{
    const QStringList invalid{
        QString(),
        QStringLiteral("v0.5.3"),
        QStringLiteral("0.5"),
        QStringLiteral("0.5.3.1"),
        QStringLiteral("0.05.3"),
        QStringLiteral("00.5.3"),
        QStringLiteral("0.5.03"),
        QStringLiteral("0.5.3-"),
        QStringLiteral("0.5.3-01"),
        QStringLiteral("0.5.3+"),
        QStringLiteral("0.5.3-rc..1"),
        QStringLiteral("0.5.3-rc_1"),
        QStringLiteral("0.5.3 "),
        QStringLiteral(" 0.5.3"),
        QStringLiteral("-1.5.3"),
        QStringLiteral("0.5.3-rc.1+"),
    };
    for (const auto &text : invalid) {
        QVERIFY2(!parseVersion(text).has_value(), qPrintable(text));
    }
}

void VersionTests::comparesNumericComponents()
{
    const auto parse = [](const char *text) { return *parseVersion(QString::fromLatin1(text)); };
    QVERIFY(compareVersions(parse("0.5.3"), parse("0.5.3")) == 0);
    QVERIFY(compareVersions(parse("0.5.3"), parse("0.5.4")) < 0);
    QVERIFY(compareVersions(parse("0.6.0"), parse("0.5.9")) > 0);
    QVERIFY(compareVersions(parse("1.0.0"), parse("0.99.99")) > 0);
    QVERIFY(compareVersions(parse("0.10.0"), parse("0.9.9")) > 0);
}

void VersionTests::comparesMultiDigitPatch()
{
    const auto older = parseVersion(QStringLiteral("0.5.9"));
    const auto newer = parseVersion(QStringLiteral("0.5.10"));
    QVERIFY(older && newer);
    // The regression the specification calls out explicitly: textual order
    // would call 0.5.9 newer than 0.5.10.
    QVERIFY(compareVersions(*older, *newer) < 0);
    QVERIFY(compareVersions(*newer, *older) > 0);
    QVERIFY(versionIsNewer(QStringLiteral("0.5.10"), QStringLiteral("0.5.9")));
    QVERIFY(!versionIsNewer(QStringLiteral("0.5.9"), QStringLiteral("0.5.10")));
}

void VersionTests::comparesPrereleaseOrdering()
{
    const auto parse = [](const char *text) { return *parseVersion(QString::fromLatin1(text)); };
    // 0.5.3 > 0.5.3-rc.1 > 0.5.3-beta
    QVERIFY(compareVersions(parse("0.5.3"), parse("0.5.3-rc.1")) > 0);
    QVERIFY(compareVersions(parse("0.5.3-rc.1"), parse("0.5.3-beta")) > 0);
    QVERIFY(compareVersions(parse("0.5.3-beta"), parse("0.5.3-alpha")) > 0);
    QVERIFY(compareVersions(parse("0.5.3-rc.2"), parse("0.5.3-rc.1")) > 0);
    QVERIFY(compareVersions(parse("0.5.3-rc.1.1"), parse("0.5.3-rc.1")) > 0);
    QVERIFY(compareVersions(parse("0.5.3-alpha.1"), parse("0.5.3-alpha")) > 0);
    QVERIFY(compareVersions(parse("0.5.3-alpha.beta"), parse("0.5.3-alpha.1")) > 0);
    // Numeric identifiers have lower precedence than alphanumeric ones.
    QVERIFY(compareVersions(parse("0.5.3-1"), parse("0.5.3-alpha")) < 0);
    QVERIFY(compareVersions(parse("0.5.3-2"), parse("0.5.3-10")) < 0);
    QVERIFY(compareVersions(parse("0.5.3-rc.1"), parse("0.5.3-rc.1")) == 0);
}

void VersionTests::treatsBuildMetadataAsEqual()
{
    const auto a = *parseVersion(QStringLiteral("0.5.3+build.9"));
    const auto b = *parseVersion(QStringLiteral("0.5.3+build.1"));
    QVERIFY(compareVersions(a, b) == 0);
    QVERIFY(!versionIsNewer(QStringLiteral("0.5.3+build.9"), QStringLiteral("0.5.3")));
}

void VersionTests::versionIsNewerHelper()
{
    bool error = true;
    QVERIFY(versionIsNewer(QStringLiteral("0.5.4"), QStringLiteral("0.5.3"), &error));
    QVERIFY(!error);
    QVERIFY(!versionIsNewer(QStringLiteral("0.5.3"), QStringLiteral("0.5.3"), &error));
    QVERIFY(!error);
    QVERIFY(!versionIsNewer(QStringLiteral("0.5.2"), QStringLiteral("0.5.3"), &error));
    QVERIFY(!error);
    QVERIFY(!versionIsNewer(QStringLiteral("not-a-version"), QStringLiteral("0.5.3"), &error));
    QVERIFY(error);
}

void VersionTests::roundTripsToString()
{
    const QByteArray original = "0.5.3-rc.1+build.5";
    const auto parsed = parseVersion(QString::fromLatin1(original));
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->toString(), QString::fromLatin1(original));
    QCOMPARE(parseVersion(QStringLiteral("0.5.3"))->toString(), QStringLiteral("0.5.3"));
}

QTEST_GUILESS_MAIN(VersionTests)
#include "VersionTests.moc"
