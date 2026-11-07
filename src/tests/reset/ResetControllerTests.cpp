#include <QtTest>

#include "ResetController.h"
#include "SystemOperations.h"
#include "ZeroTierClient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <functional>
#include <memory>

using namespace vusiif::zto::reset;

namespace {

void writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();
}

QString notificationMessage(const QSignalSpy &spy)
{
    if (spy.isEmpty()) return {};
    return spy.at(spy.count() - 1).at(0).toString();
}

bool notificationError(const QSignalSpy &spy)
{
    if (spy.isEmpty()) return false;
    return spy.at(spy.count() - 1).at(1).toBool();
}

class FakeOperations : public vusiif::zto::ui::SystemOperations {
public:
    struct Call {
        QString label;
        QString script;
    };

    QList<Call> calls;
    QStringList outputChunks;
    bool autoComplete = true;
    bool result = true;
    QStringList *eventLog = nullptr;
    std::function<void(bool)> pending;

    void runElevatedScript(const QString &label, const QString &script,
                           std::function<void(bool)> finished) override
    {
        calls.append({label, script});
        if (eventLog) eventLog->append(QStringLiteral("operation"));
        for (const auto &chunk : outputChunks) emit output(chunk);
        if (autoComplete) {
            if (finished) finished(result);
        } else {
            pending = std::move(finished);
        }
    }

    void complete(bool ok)
    {
        auto callback = std::move(pending);
        pending = {};
        if (callback) callback(ok);
    }
};

} // namespace

class ResetControllerTests : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup();

    void confirmationCancelMakesNoChanges();
    void blockedProviderPreventsConfirmation();
    void elevationCancelledMakesNoChanges();
    void stopFailureReportsNoDeletion();
    void partialCleanupReportsStage();
    void startFailureReportsStartStage();
    void successOrderAndRefresh();
    void invalidLocalConfigStopsBeforeElevation();
    void emptyPlanDoesNotStopService();
    void secondClickWhileBusyIgnored();
    void serviceNotInstalledMessage();
    void localConfigRelayIsHandledByScript();
    void emptyLocalConfigAccepted();

private:
    void writeRootFile(const QString &relative, const QByteArray &bytes);
    bool rootFileExists(const QString &relative) const;

    std::unique_ptr<QTemporaryDir> m_directory;
    std::unique_ptr<FakeOperations> m_operations;
    std::unique_ptr<ResetController> m_controller;
    QString m_root;
    QStringList m_events;
};

void ResetControllerTests::init()
{
    m_directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/zto-reset-test-XXXXXX"));
    QVERIFY(m_directory->isValid());
    qputenv("ZT_TEST_DATA_DIRECTORY", m_directory->path().toUtf8());
    m_root = vusiif::zto::zerotier::systemHomeDirectory();
    QDir().mkpath(m_root + QStringLiteral("/networks.d"));
    QDir().mkpath(m_root + QStringLiteral("/peers.d"));
    QDir().mkpath(m_root + QStringLiteral("/moons.d"));
    writeRootFile(QStringLiteral("networks.d/0123456789abcdef.conf"), QByteArray("network"));
    writeRootFile(QStringLiteral("peers.d/0123456789.peer"), QByteArray("peer"));
    writeRootFile(QStringLiteral("moons.d/0123456789abcdef.moon"), QByteArray("moon"));
    writeRootFile(QStringLiteral("planet"), QByteArray("planet"));
    writeRootFile(QStringLiteral("identity.secret"), QByteArray("identity-secret"));
    writeRootFile(QStringLiteral("identity.public"), QByteArray("identity-public"));
    writeRootFile(QStringLiteral("authtoken.secret"), QByteArray("token"));
    writeRootFile(QStringLiteral("zerotier-one.port"), QByteArray("9993"));
    writeRootFile(QStringLiteral("local.conf"),
                  QByteArray(R"({"settings":{"allowTcpFallbackRelay":true,"forceTcpRelay":false,"tcpFallbackRelay":"1.2.3.4/9443","primaryPort":19993},"custom":{"keep":1}})"));
    m_operations = std::make_unique<FakeOperations>();
    m_operations->eventLog = &m_events;
    m_controller = std::make_unique<ResetController>(*m_operations);
    m_events.clear();
}

void ResetControllerTests::cleanup()
{
    m_controller.reset();
    m_operations.reset();
    m_directory.reset();
    qputenv("ZT_TEST_DATA_DIRECTORY", QByteArray());
}

void ResetControllerTests::writeRootFile(const QString &relative, const QByteArray &bytes)
{
    writeFile(m_root + QLatin1Char('/') + relative, bytes);
}

bool ResetControllerTests::rootFileExists(const QString &relative) const
{
    return QFileInfo::exists(m_root + QLatin1Char('/') + relative);
}

void ResetControllerTests::confirmationCancelMakesNoChanges()
{
    QSignalSpy confirmation(m_controller.get(), &ResetController::confirmationRequested);
    QSignalSpy began(m_controller.get(), &ResetController::interruptionBegan);
    QSignalSpy ended(m_controller.get(), &ResetController::interruptionEnded);
    QSignalSpy notifications(m_controller.get(), &ResetController::notificationRequested);
    QSignalSpy busyChanges(m_controller.get(), &ResetController::busyChanged);

    m_controller->requestReset();
    QCOMPARE(confirmation.count(), 1);
    QVERIFY(confirmation.at(0).at(1).toString().contains(QStringLiteral("退出所有")));
    QVERIFY(confirmation.at(0).at(1).toString().contains(QStringLiteral("不会重启 Z2")));
    QVERIFY(confirmation.at(0).at(1).toString().contains(QStringLiteral("暂时无法连接")));
    m_events.clear();

    m_controller->confirm(false);
    QCOMPARE(m_operations->calls.size(), 0);
    QCOMPARE(began.count(), 0);
    QCOMPARE(ended.count(), 0);
    QCOMPARE(busyChanges.count(), 0);
    QVERIFY(!m_controller->busy());
    QCOMPARE(notifications.count(), 1);
    QCOMPARE(notificationError(notifications), false);
    QVERIFY(notificationMessage(notifications).contains(QStringLiteral("已取消")));
    QVERIFY(m_controller->lastError().isEmpty());
    QVERIFY(rootFileExists(QStringLiteral("networks.d/0123456789abcdef.conf")));
    QVERIFY(rootFileExists(QStringLiteral("planet")));
}

void ResetControllerTests::blockedProviderPreventsConfirmation()
{
    QSignalSpy confirmation(m_controller.get(), &ResetController::confirmationRequested);
    QSignalSpy notifications(m_controller.get(), &ResetController::notificationRequested);
    m_controller->setBlockedProvider([] { return true; });
    QVERIFY(!m_controller->canReset());

    m_controller->requestReset();
    QCOMPARE(confirmation.count(), 0);
    QCOMPARE(m_operations->calls.size(), 0);
    QCOMPARE(notifications.count(), 1);
    QCOMPARE(notificationError(notifications), true);

    m_controller->setBlockedProvider([] { return false; });
    QVERIFY(m_controller->canReset());
    m_controller->requestReset();
    QCOMPARE(confirmation.count(), 1);
}

void ResetControllerTests::elevationCancelledMakesNoChanges()
{
    QSignalSpy began(m_controller.get(), &ResetController::interruptionBegan);
    QSignalSpy ended(m_controller.get(), &ResetController::interruptionEnded);
    QSignalSpy notifications(m_controller.get(), &ResetController::notificationRequested);
    QSignalSpy busyChanges(m_controller.get(), &ResetController::busyChanged);
    m_operations->result = false; // UAC/osascript cancelled: no output at all.

    m_controller->requestReset();
    m_controller->confirm(true);

    QCOMPARE(m_operations->calls.size(), 1);
    QCOMPARE(m_operations->calls.first().label, QStringLiteral("重置 ZeroTier 本地状态"));
    QCOMPARE(began.count(), 1);
    QCOMPARE(ended.count(), 1);
    QCOMPARE(busyChanges.count(), 2); // true -> false
    QVERIFY(!m_controller->busy());
    QVERIFY(m_controller->lastError().contains(QStringLiteral("未做任何更改")));
    QCOMPARE(notifications.count(), 1);
    QCOMPARE(notificationError(notifications), true);
    QVERIFY(notificationMessage(notifications).contains(QStringLiteral("授权被取消")));
    QVERIFY(rootFileExists(QStringLiteral("networks.d/0123456789abcdef.conf")));
    QVERIFY(rootFileExists(QStringLiteral("moons.d/0123456789abcdef.moon")));
    QVERIFY(rootFileExists(QStringLiteral("planet")));
    QVERIFY(rootFileExists(QStringLiteral("identity.secret")));
}

void ResetControllerTests::stopFailureReportsNoDeletion()
{
    QSignalSpy began(m_controller.get(), &ResetController::interruptionBegan);
    QSignalSpy ended(m_controller.get(), &ResetController::interruptionEnded);
    QSignalSpy notifications(m_controller.get(), &ResetController::notificationRequested);
    m_operations->result = false;
    m_operations->outputChunks = {QStringLiteral(
        "ZTO-RESET stage=elevated status=ok\n"
        "ZTO-RESET stage=stop status=failed message=service did not stop\n"
        "ZTO-RESET stage=recovery status=ok\n")};

    m_controller->requestReset();
    m_controller->confirm(true);

    QCOMPARE(m_operations->calls.size(), 1);
    QCOMPARE(began.count(), 1);
    QCOMPARE(ended.count(), 1);
    QVERIFY(!m_controller->busy());
    const auto message = notificationMessage(notifications);
    QVERIFY2(message.contains(QStringLiteral("停止 ZeroTier 服务失败")), qPrintable(message));
    QVERIFY(message.contains(QStringLiteral("未删除任何文件")));
    QVERIFY(message.contains(QStringLiteral("已尝试恢复 ZeroTier 服务")));
    QCOMPARE(notificationError(notifications), true);
    QVERIFY(m_controller->lastError().contains(QStringLiteral("停止")));
    // The fake never executed the script, and the stop stage precedes cleanup.
    const auto &script = m_operations->calls.first().script;
    const int stop = script.indexOf(QStringLiteral("stage=stop status=ok"));
    const int remove = script.indexOf(QStringLiteral("Remove-Item -LiteralPath $target"));
    QVERIFY(stop >= 0 && remove > stop);
    QVERIFY(rootFileExists(QStringLiteral("networks.d/0123456789abcdef.conf")));
    QVERIFY(rootFileExists(QStringLiteral("identity.secret")));
}

void ResetControllerTests::partialCleanupReportsStage()
{
    QSignalSpy notifications(m_controller.get(), &ResetController::notificationRequested);
    m_operations->result = false;
    m_operations->outputChunks = {QStringLiteral(
        "ZTO-RESET stage=elevated status=ok\n"
        "ZTO-RESET stage=stop status=ok\n"
        "ZTO-RESET stage=cleanup status=failed message=Remove-Item peers.d/0123456789.peer: access denied\n"
        "ZTO-RESET stage=recovery status=ok\n")};

    m_controller->requestReset();
    m_controller->confirm(true);

    const auto message = notificationMessage(notifications);
    QVERIFY2(message.contains(QStringLiteral("清理本地状态失败")), qPrintable(message));
    QVERIFY(message.contains(QStringLiteral("部分文件可能已删除")));
    QVERIFY(message.contains(QStringLiteral("peers.d/0123456789.peer")));
    QVERIFY(message.contains(QStringLiteral("已尝试恢复 ZeroTier 服务")));
    QCOMPARE(notificationError(notifications), true);
}

void ResetControllerTests::startFailureReportsStartStage()
{
    QSignalSpy notifications(m_controller.get(), &ResetController::notificationRequested);
    m_operations->result = false;
    m_operations->outputChunks = {QStringLiteral(
        "ZTO-RESET stage=elevated status=ok\n"
        "ZTO-RESET stage=stop status=ok\n"
        "ZTO-RESET stage=cleanup status=ok\n"
        "ZTO-RESET stage=localconf status=ok\n"
        "ZTO-RESET stage=start status=failed message=ZeroTier service is running but its local API is unavailable\n"
        "ZTO-RESET stage=recovery status=failed message=cannot start service\n")};

    m_controller->requestReset();
    m_controller->confirm(true);

    const auto message = notificationMessage(notifications);
    QVERIFY2(message.contains(QStringLiteral("启动或本地 API 就绪验证失败")), qPrintable(message));
    QVERIFY(message.contains(QStringLiteral("恢复 ZeroTier 服务失败")));
    QVERIFY(message.contains(QStringLiteral("local API is unavailable")));
    QCOMPARE(notificationError(notifications), true);
}

void ResetControllerTests::successOrderAndRefresh()
{
    QSignalSpy confirmation(m_controller.get(), &ResetController::confirmationRequested);
    QSignalSpy notifications(m_controller.get(), &ResetController::notificationRequested);
    QSignalSpy busyChanges(m_controller.get(), &ResetController::busyChanged);
    QObject::connect(m_controller.get(), &ResetController::interruptionBegan, this,
                     [this] { m_events.append(QStringLiteral("began")); });
    QObject::connect(m_controller.get(), &ResetController::interruptionEnded, this,
                     [this] { m_events.append(QStringLiteral("ended")); });
    QObject::connect(m_controller.get(), &ResetController::notificationRequested, this,
                     [this](const QString &, bool) { m_events.append(QStringLiteral("notification")); });
    m_operations->result = true;
    m_operations->outputChunks = {QStringLiteral(
        "ZTO-RESET stage=elevated status=ok\n"
        "ZTO-RESET stage=stop status=ok\n"
        "ZTO-RESET stage=cleanup status=ok\n"
        "ZTO-RESET stage=localconf status=ok\n"
        "ZTO-RESET stage=start status=ok\n"
        "ZTO-RESET stage=done status=ok\n")};

    m_controller->requestReset();
    QCOMPARE(confirmation.count(), 1);
    m_events.clear();
    m_controller->confirm(true);

    QCOMPARE(m_operations->calls.size(), 1);
    QCOMPARE(m_events, QStringList({QStringLiteral("began"), QStringLiteral("operation"),
                                    QStringLiteral("ended"), QStringLiteral("notification")}));
    QCOMPARE(busyChanges.count(), 2);
    QVERIFY(!m_controller->busy());
    QVERIFY(m_controller->canReset());
    QVERIFY(m_controller->lastError().isEmpty());
    QCOMPARE(notifications.count(), 1);
    QCOMPARE(notificationError(notifications), false);
    const auto message = notificationMessage(notifications);
    QVERIFY2(message.contains(QStringLiteral("已清除 4 个本地状态文件")), qPrintable(message));
    QVERIFY(message.contains(QStringLiteral("TCP 回退中转设置")));
    QVERIFY(message.contains(QStringLiteral("ZeroTier 服务已重启")));
    QVERIFY(!message.contains(QStringLiteral("成员数量")));
    QVERIFY(m_controller->status().contains(QStringLiteral("已重启")));

    const auto &script = m_operations->calls.first().script;
    QVERIFY(script.contains(QStringLiteral("zerotier-one.port")));
    QVERIFY(script.contains(QStringLiteral("/status")));
    QVERIFY(script.contains(QStringLiteral("Start-Service -Name 'ZeroTierOneService'")));
    QVERIFY(script.contains(QStringLiteral("local.conf changed since it was validated")));
}

void ResetControllerTests::invalidLocalConfigStopsBeforeElevation()
{
    writeRootFile(QStringLiteral("local.conf"), QByteArray("{"));
    QSignalSpy began(m_controller.get(), &ResetController::interruptionBegan);
    QSignalSpy ended(m_controller.get(), &ResetController::interruptionEnded);
    QSignalSpy notifications(m_controller.get(), &ResetController::notificationRequested);

    m_controller->requestReset();
    m_controller->confirm(true);

    QCOMPARE(m_operations->calls.size(), 0);
    QCOMPARE(began.count(), 1);
    QCOMPARE(ended.count(), 1);
    QVERIFY(!m_controller->busy());
    QCOMPARE(notifications.count(), 1);
    QCOMPARE(notificationError(notifications), true);
    QVERIFY(notificationMessage(notifications).contains(QStringLiteral("未做任何更改")));
    QVERIFY(notificationMessage(notifications).contains(QStringLiteral("local.conf")));
    QVERIFY(m_controller->lastError().contains(QStringLiteral("合法 JSON")));
    QVERIFY(rootFileExists(QStringLiteral("networks.d/0123456789abcdef.conf")));
}

void ResetControllerTests::emptyPlanDoesNotStopService()
{
    QFile::remove(m_root + QStringLiteral("/networks.d/0123456789abcdef.conf"));
    QFile::remove(m_root + QStringLiteral("/peers.d/0123456789.peer"));
    QFile::remove(m_root + QStringLiteral("/moons.d/0123456789abcdef.moon"));
    QFile::remove(m_root + QStringLiteral("/planet"));
    QFile::remove(m_root + QStringLiteral("/local.conf"));
    QSignalSpy began(m_controller.get(), &ResetController::interruptionBegan);
    QSignalSpy ended(m_controller.get(), &ResetController::interruptionEnded);
    QSignalSpy notifications(m_controller.get(), &ResetController::notificationRequested);

    m_controller->requestReset();
    m_controller->confirm(true);

    QCOMPARE(m_operations->calls.size(), 0);
    QCOMPARE(began.count(), 1);
    QCOMPARE(ended.count(), 1);
    QCOMPARE(notifications.count(), 1);
    QCOMPARE(notificationError(notifications), false);
    QVERIFY(notificationMessage(notifications).contains(QStringLiteral("没有发现需要清理")));
    QVERIFY(notificationMessage(notifications).contains(QStringLiteral("未停止 ZeroTier 服务")));
}

void ResetControllerTests::secondClickWhileBusyIgnored()
{
    QSignalSpy confirmation(m_controller.get(), &ResetController::confirmationRequested);
    QSignalSpy ended(m_controller.get(), &ResetController::interruptionEnded);
    m_operations->autoComplete = false;
    m_operations->outputChunks = {QStringLiteral(
        "ZTO-RESET stage=elevated status=ok\n"
        "ZTO-RESET stage=stop status=ok\n"
        "ZTO-RESET stage=cleanup status=ok\n"
        "ZTO-RESET stage=localconf status=ok\n"
        "ZTO-RESET stage=start status=ok\n"
        "ZTO-RESET stage=done status=ok\n")};

    m_controller->requestReset();
    QCOMPARE(confirmation.count(), 1);
    m_controller->confirm(true);
    QVERIFY(m_controller->busy());
    QCOMPARE(m_operations->calls.size(), 1);

    m_controller->requestReset();
    m_controller->requestReset();
    m_controller->confirm(true);
    QCOMPARE(confirmation.count(), 1);
    QCOMPARE(m_operations->calls.size(), 1);
    QCOMPARE(ended.count(), 0);

    m_operations->complete(true);
    QCOMPARE(ended.count(), 1);
    QVERIFY(!m_controller->busy());
}

void ResetControllerTests::serviceNotInstalledMessage()
{
    QSignalSpy notifications(m_controller.get(), &ResetController::notificationRequested);
    m_operations->result = false;
    m_operations->outputChunks = {QStringLiteral(
        "ZTO-RESET stage=elevated status=ok\n"
        "ZTO-RESET stage=stop status=failed message=not-installed\n")};

    m_controller->requestReset();
    m_controller->confirm(true);

    const auto message = notificationMessage(notifications);
    QVERIFY2(message.contains(QStringLiteral("未检测到已安装的 ZeroTier 服务")), qPrintable(message));
    QVERIFY(message.contains(QStringLiteral("未做任何更改")));
    QVERIFY(!message.contains(QStringLiteral("已尝试恢复")));
    QCOMPARE(notificationError(notifications), true);
}

void ResetControllerTests::localConfigRelayIsHandledByScript()
{
    m_controller->requestReset();
    m_controller->confirm(true);
    QCOMPARE(m_operations->calls.size(), 1);
    const auto &script = m_operations->calls.first().script;
    QVERIFY(script.contains(QStringLiteral("local.conf changed since it was validated")));
    QVERIFY(script.contains(QStringLiteral("$localExpected")));
    QVERIFY(script.contains(QStringLiteral("$localTarget")));
    // The merged bytes are passed as a file, not inlined into the command line.
    QVERIFY(!script.contains(QStringLiteral("tcpFallbackRelay")));
}

void ResetControllerTests::emptyLocalConfigAccepted()
{
    writeRootFile(QStringLiteral("local.conf"), QByteArray());
    QSignalSpy notifications(m_controller.get(), &ResetController::notificationRequested);
    m_operations->result = true;
    m_operations->outputChunks = {QStringLiteral(
        "ZTO-RESET stage=elevated status=ok\n"
        "ZTO-RESET stage=stop status=ok\n"
        "ZTO-RESET stage=cleanup status=ok\n"
        "ZTO-RESET stage=localconf status=ok\n"
        "ZTO-RESET stage=start status=ok\n"
        "ZTO-RESET stage=done status=ok\n")};

    m_controller->requestReset();
    m_controller->confirm(true);

    QCOMPARE(m_operations->calls.size(), 1);
    QVERIFY(!m_operations->calls.first().script.contains(QStringLiteral("local.conf changed since it was validated")));
    QCOMPARE(notificationError(notifications), false);
    QVERIFY(!notificationMessage(notifications).contains(QStringLiteral("TCP 回退中转设置")));
    QVERIFY(m_controller->lastError().isEmpty());
    QVERIFY(!m_controller->busy());
}

QTEST_GUILESS_MAIN(ResetControllerTests)
#include "ResetControllerTests.moc"
