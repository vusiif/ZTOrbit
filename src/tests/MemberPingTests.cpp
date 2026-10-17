#include "MemberPing.h"
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using namespace vusiif::ztgui::zerotier;
class MemberPingTests : public QObject {
    Q_OBJECT
private slots:
    void cleanup() {
        qunsetenv("ZT_TEST_PING_MODE"); qunsetenv("ZT_TEST_PING_DELAY"); qunsetenv("ZT_TEST_PING_LOG");
    }
    void replyParsing_data() {
        QTest::addColumn<QByteArray>("output"); QTest::addColumn<QString>("expected");
        QTest::newRow("windows") << QByteArray("Reply from 10.0.0.2: bytes=32 time=23ms TTL=128\n") << QStringLiteral("23 ms");
        QTest::newRow("chinese-windows") << QStringLiteral("来自 10.0.0.2 的回复: 字节=32 时间<1ms TTL=128\n").toLocal8Bit() << QStringLiteral("<1 ms");
        QTest::newRow("unix") << QByteArray("64 bytes from 10.0.0.2: icmp_seq=1 ttl=64 time=0.123 ms\n") << QStringLiteral("0.123 ms");
        QTest::newRow("decimal-comma") << QByteArray("64 bytes from 10.0.0.2: icmp_seq=1 ttl=64 time=0,123 ms\n") << QStringLiteral("0.123 ms");
        QTest::newRow("timeout") << QByteArray("Request timed out.\n") << QString();
        QTest::newRow("unreachable-with-zero-exit") << QByteArray("Reply from 10.0.0.1: Destination host unreachable.\nMinimum = 0ms, Maximum = 0ms\n") << QString();
        QTest::newRow("ttl-expired") << QByteArray("Reply from 10.0.0.1: TTL expired in transit.\n") << QString();
    }
    void replyParsing() {
        QFETCH(QByteArray, output); QFETCH(QString, expected); QCOMPARE(parsePingReply(output), expected);
    }
    void asynchronousReplyAndUnreachable() {
        qputenv("ZT_TEST_PING_DELAY", "150");
        int calls = 0; PingResult result;
        pingMember(this, QHostAddress::LocalHost, [&](auto value) { result=value; ++calls; }, FAKE_PING);
        QCOMPARE(calls, 0); QTRY_COMPARE(calls, 1); QCOMPARE(result.text, QString("12 ms"));
        qputenv("ZT_TEST_PING_MODE", "unreachable");
        pingMember(this, QHostAddress::LocalHost, [&](auto value) { result=value; ++calls; }, FAKE_PING);
        QTRY_COMPARE(calls, 2); QCOMPARE(result.text, QStringLiteral("不可达"));
    }
    void deadlineAndCancellation() {
        qputenv("ZT_TEST_PING_MODE", "hang");
        int calls = 0; PingResult result; QElapsedTimer elapsed; elapsed.start();
        pingMember(this, QHostAddress::LocalHost, [&](auto value) { result=value; ++calls; }, FAKE_PING);
        QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 5000);
        QVERIFY(elapsed.elapsed() >= 3500); QVERIFY(elapsed.elapsed() < 5000);
        QCOMPARE(result.text, QStringLiteral("不可达")); QVERIFY(result.detail.contains(QStringLiteral("超时")));
        auto *context = new QObject;
        pingMember(context, QHostAddress::LocalHost, [&](auto) { ++calls; }, FAKE_PING);
        QTRY_VERIFY(context->findChild<QProcess *>() && context->findChild<QProcess *>()->state() == QProcess::Running);
        delete context; QTest::qWait(100); QCOMPARE(calls, 1);
    }
    void invalidAddressAndMissingCommand() {
        int calls = 0; PingResult result;
        pingMember(this, {}, [&](auto value) { result=value; ++calls; }, FAKE_PING);
        QTRY_COMPARE(calls, 1); QCOMPARE(result.text, QStringLiteral("—"));
        pingMember(this, QHostAddress::LocalHost, [&](auto value) { result=value; ++calls; }, "missing-ztgui-ping-command");
        QTRY_COMPARE(calls, 2); QCOMPARE(result.text, QStringLiteral("检测失败"));
    }
    void systemPingLoopback() {
        int calls = 0; PingResult result;
        pingMember(this, QHostAddress::LocalHost, [&](auto value) { result=value; ++calls; });
        QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 5000);
        QVERIFY2(result.text.endsWith(" ms"), qPrintable(result.text + ": " + result.detail));
    }
};
QTEST_GUILESS_MAIN(MemberPingTests)
#include "MemberPingTests.moc"
