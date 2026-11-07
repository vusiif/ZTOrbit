#include "TestSupport.h"
#include "UpdaterEngine.h"
#include "UpdaterPlan.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using namespace vusiif::zto::update::updater;

namespace {

struct Tree {
    QTemporaryDir temporary;
    QString install;
    QString staging;
    QString runtime;
    QString transaction;
    QString logFile;
    QString resultFile;
    QString planFile;

    bool initialize(QString &error)
    {
        if (!temporary.isValid()) {
            error = QStringLiteral("no temporary directory");
            return false;
        }
        install = temporary.filePath(QStringLiteral("install"));
        staging = temporary.filePath(QStringLiteral("staging"));
        runtime = temporary.filePath(QStringLiteral("runtime"));
        transaction = temporary.filePath(QStringLiteral("transaction"));
        logFile = temporary.filePath(QStringLiteral("log/updater.log"));
        resultFile = temporary.filePath(QStringLiteral("last-result.txt"));
        planFile = temporary.filePath(QStringLiteral("update.plan"));
        return QDir().mkpath(install) && QDir().mkpath(staging) && QDir().mkpath(
            QFileInfo(runtime).absolutePath());
    }
};

Plan buildPlan(const Tree &tree, const QList<PlanFile> &files, const QStringList &removals,
               qint64 parentPid, const QString &restart = QStringLiteral("restart_probe.exe"))
{
    Plan plan;
    plan.packageVersion = "0.5.4";
    plan.installDir = QDir::toNativeSeparators(tree.install).toStdString();
    plan.stagingDir = QDir::toNativeSeparators(tree.staging).toStdString();
    plan.transactionDir = QDir::toNativeSeparators(tree.transaction).toStdString();
    plan.runtimeDir = QDir::toNativeSeparators(tree.runtime).toStdString();
    plan.logFile = QDir::toNativeSeparators(tree.logFile).toStdString();
    plan.resultFile = QDir::toNativeSeparators(tree.resultFile).toStdString();
    plan.parentPid = static_cast<std::uint64_t>(parentPid);
    plan.restartRelative = restart.toStdString();
    plan.files = std::vector<PlanFile>(files.begin(), files.end());
    for (const auto &removal : removals) plan.removals.push_back(removal.toStdString());
    return plan;
}

void stage(const Tree &tree, const QString &relative, const QByteArray &data, PlanFile &file)
{
    QString error;
    zto_update_test::writeFile(QDir(tree.staging).filePath(relative), data, error);
    file.path = relative.toStdString();
    file.size = static_cast<std::uint64_t>(data.size());
    file.sha256 = zto_update_test::sha256Hex(data).toStdString();
}

bool writePlanFile(const Tree &tree, const Plan &plan, QString &error)
{
    std::string text;
    std::string planError;
    if (!serializePlan(plan, text, planError)) {
        error = QString::fromStdString(planError);
        return false;
    }
    return zto_update_test::writeFile(tree.planFile, QByteArray::fromStdString(text), error);
}

qint64 deadProcessId()
{
    QProcess process;
    process.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("exit 0")});
    if (!process.waitForStarted(5000)) return 0;
    const auto pid = process.processId();
    process.waitForFinished(5000);
    return pid;
}

int runHelper(const QStringList &arguments, int timeoutMs = 20000)
{
    QProcess process;
    process.setProgram(QStringLiteral(Z2_UPDATER_EXE));
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(5000)) return -1;
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(5000);
        return -2;
    }
    return process.exitCode();
}

QString resultText(const Tree &tree)
{
    return QString::fromUtf8(zto_update_test::readFile(tree.resultFile));
}

} // namespace

class EngineTests : public QObject {
    Q_OBJECT
private slots:
    void directSuccessReplacesAndCleans();
    void directFailureRestoresByteForByte();
    void realHelperSuccess();
    void realHelperWaitTimeout();
    void realHelperRestartsOnlyOnSuccess();
    void realHelperSuppressesRestart();
    void realHelperRejectsUnsafePlan();
    void recoveryRestoresInterruptedTransaction();
    void recoveryCommittedOnlyCleansUp();
    void detectsSelfReplacement();
};

void EngineTests::directSuccessReplacesAndCleans()
{
    Tree tree;
    QString error;
    QVERIFY2(tree.initialize(error), qPrintable(error));
    QVERIFY(zto_update_test::writeFile(QDir(tree.install).filePath(QStringLiteral("a.dll")),
                                       "old-a", error));
    PlanFile fileA;
    stage(tree, QStringLiteral("a.dll"), "new-a", fileA);
    PlanFile fileB;
    stage(tree, QStringLiteral("nested/b.dll"), "new-b", fileB);
    auto plan = buildPlan(tree, {fileA, fileB}, {QStringLiteral("old-only.dll")}, 4242);
    QVERIFY(zto_update_test::writeFile(QDir(tree.install).filePath(QStringLiteral("old-only.dll")),
                                       "remove me", error));
    EngineOptions options;
    options.noRestart = true;
    const auto result = runTransaction(plan, options);
    QVERIFY2(result.ok, qPrintable(QString::fromStdString(result.message)));
    QVERIFY(result.restartSuppressed);
    QVERIFY(result.backupRemoved);
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(zto_update_test::readFile(QDir(tree.install).filePath(QStringLiteral("a.dll"))),
             QByteArrayLiteral("new-a"));
    QCOMPARE(zto_update_test::readFile(QDir(tree.install).filePath(QStringLiteral("nested/b.dll"))),
             QByteArrayLiteral("new-b"));
    QVERIFY(!QFileInfo::exists(QDir(tree.install).filePath(QStringLiteral("old-only.dll"))));
    QVERIFY(!QFileInfo::exists(tree.transaction));
    QVERIFY(resultText(tree).contains(QStringLiteral("result=ok")));
}

void EngineTests::directFailureRestoresByteForByte()
{
    Tree tree;
    QString error;
    QVERIFY2(tree.initialize(error), qPrintable(error));
    QVERIFY(zto_update_test::writeFile(QDir(tree.install).filePath(QStringLiteral("a.dll")),
                                       "old-a-bytes", error));
    QVERIFY(zto_update_test::writeFile(QDir(tree.install).filePath(QStringLiteral("b.dll")),
                                       "old-b-bytes", error));
    PlanFile fileA;
    stage(tree, QStringLiteral("a.dll"), "new-a", fileA);
    PlanFile fileB;
    stage(tree, QStringLiteral("b.dll"), "new-b", fileB);
    auto plan = buildPlan(tree, {fileA, fileB}, {}, 4242);
    EngineOptions options;
    options.noRestart = true;
    options.failAfterReplacements = 1; // a.dll is replaced, then the transaction fails
    const auto result = runTransaction(plan, options);
    QVERIFY(!result.ok);
    QVERIFY(result.rolledBack);
    QVERIFY(!result.restartStarted);
    QCOMPARE(result.step, std::string("replace"));
    QCOMPARE(zto_update_test::readFile(QDir(tree.install).filePath(QStringLiteral("a.dll"))),
             QByteArrayLiteral("old-a-bytes"));
    QCOMPARE(zto_update_test::readFile(QDir(tree.install).filePath(QStringLiteral("b.dll"))),
             QByteArrayLiteral("old-b-bytes"));
    QVERIFY(QFileInfo::exists(tree.transaction));
    QVERIFY(resultText(tree).contains(QStringLiteral("result=failed")));
    QVERIFY(resultText(tree).contains(QStringLiteral("restart=not-requested")));
    // The journal must be intact for the next-start recovery pass.
    QVERIFY(QFileInfo::exists(QDir(tree.transaction).filePath(QStringLiteral("journal.txt"))));
}

void EngineTests::realHelperSuccess()
{
    Tree tree;
    QString error;
    QVERIFY2(tree.initialize(error), qPrintable(error));
    QVERIFY(zto_update_test::writeFile(QDir(tree.install).filePath(QStringLiteral("a.dll")),
                                       "old", error));
    PlanFile fileA;
    stage(tree, QStringLiteral("a.dll"), "new-from-helper", fileA);
    const auto pid = deadProcessId();
    QVERIFY(pid > 0);
    const auto plan = buildPlan(tree, {fileA}, {}, pid);
    QVERIFY2(writePlanFile(tree, plan, error), qPrintable(error));
    QCOMPARE(runHelper({QStringLiteral("--plan"), tree.planFile, QStringLiteral("--no-restart")}), 0);
    QCOMPARE(zto_update_test::readFile(QDir(tree.install).filePath(QStringLiteral("a.dll"))),
             QByteArrayLiteral("new-from-helper"));
    QVERIFY(!QFileInfo::exists(tree.transaction));
    QVERIFY(resultText(tree).contains(QStringLiteral("result=ok")));
    QVERIFY(resultText(tree).contains(QStringLiteral("restart=suppressed")));
}

void EngineTests::realHelperWaitTimeout()
{
    Tree tree;
    QString error;
    QVERIFY2(tree.initialize(error), qPrintable(error));
    QVERIFY(zto_update_test::writeFile(QDir(tree.install).filePath(QStringLiteral("a.dll")),
                                       "old", error));
    PlanFile fileA;
    stage(tree, QStringLiteral("a.dll"), "new", fileA);
    // Our own (alive) process is the parent and never exits during the test.
    const auto plan = buildPlan(tree, {fileA}, {}, QCoreApplication::applicationPid());
    QVERIFY2(writePlanFile(tree, plan, error), qPrintable(error));
    QCOMPARE(runHelper({QStringLiteral("--plan"), tree.planFile, QStringLiteral("--no-restart"),
                        QStringLiteral("--wait-timeout"), QStringLiteral("1")}),
             4);
    QCOMPARE(zto_update_test::readFile(QDir(tree.install).filePath(QStringLiteral("a.dll"))),
             QByteArrayLiteral("old"));
    QVERIFY(resultText(tree).contains(QStringLiteral("step=wait")));
    QVERIFY(!QFileInfo::exists(tree.transaction));
}

void EngineTests::realHelperRestartsOnlyOnSuccess()
{
    Tree tree;
    QString error;
    QVERIFY2(tree.initialize(error), qPrintable(error));
    QVERIFY(QFile::copy(QStringLiteral(RESTART_PROBE_EXE),
                        QDir(tree.install).filePath(QStringLiteral("restart_probe.exe"))));
    PlanFile fileA;
    stage(tree, QStringLiteral("a.dll"), "new", fileA);
    const auto pid = deadProcessId();
    QVERIFY(pid > 0);
    const auto plan = buildPlan(tree, {fileA}, {}, pid, QStringLiteral("restart_probe.exe"));
    QVERIFY2(writePlanFile(tree, plan, error), qPrintable(error));
    QCOMPARE(runHelper({QStringLiteral("--plan"), tree.planFile}), 0);
    QVERIFY(resultText(tree).contains(QStringLiteral("restart=started")));
    QTRY_VERIFY_WITH_TIMEOUT(
        QFileInfo::exists(QDir(tree.install).filePath(QStringLiteral("restart-probe-ran.txt"))), 5000);
}

void EngineTests::realHelperSuppressesRestart()
{
    Tree tree;
    QString error;
    QVERIFY2(tree.initialize(error), qPrintable(error));
    QVERIFY(QFile::copy(QStringLiteral(RESTART_PROBE_EXE),
                        QDir(tree.install).filePath(QStringLiteral("restart_probe.exe"))));
    PlanFile fileA;
    stage(tree, QStringLiteral("a.dll"), "new", fileA);
    const auto pid = deadProcessId();
    QVERIFY(pid > 0);
    const auto plan = buildPlan(tree, {fileA}, {}, pid, QStringLiteral("restart_probe.exe"));
    QVERIFY2(writePlanFile(tree, plan, error), qPrintable(error));
    QCOMPARE(runHelper({QStringLiteral("--plan"), tree.planFile, QStringLiteral("--no-restart")}), 0);
    QTest::qWait(500);
    QVERIFY(!QFileInfo::exists(QDir(tree.install).filePath(QStringLiteral("restart-probe-ran.txt"))));
    QVERIFY(resultText(tree).contains(QStringLiteral("restart=suppressed")));
}

void EngineTests::realHelperRejectsUnsafePlan()
{
    Tree tree;
    QString error;
    QVERIFY2(tree.initialize(error), qPrintable(error));
    QVERIFY(zto_update_test::writeFile(QDir(tree.install).filePath(QStringLiteral("a.dll")),
                                       "old", error));
    Plan plan;
    plan.installDir = QDir::toNativeSeparators(tree.install).toStdString();
    plan.stagingDir = QDir::toNativeSeparators(tree.staging).toStdString();
    plan.transactionDir = QDir::toNativeSeparators(tree.transaction).toStdString();
    plan.runtimeDir = QDir::toNativeSeparators(tree.runtime).toStdString();
    plan.logFile = QDir::toNativeSeparators(tree.logFile).toStdString();
    plan.resultFile = QDir::toNativeSeparators(tree.resultFile).toStdString();
    plan.parentPid = 1;
    plan.restartRelative = "restart_probe.exe";
    QVERIFY(zto_update_test::writeFile(tree.planFile,
                                       QByteArray("z2-update-plan=1\n"
                                                  "install-dir=" + QByteArray::fromStdString(plan.installDir) + "\n"
                                                  "staging-dir=" + QByteArray::fromStdString(plan.stagingDir) + "\n"
                                                  "transaction-dir=" + QByteArray::fromStdString(plan.transactionDir) + "\n"
                                                  "runtime-dir=" + QByteArray::fromStdString(plan.runtimeDir) + "\n"
                                                  "log-file=" + QByteArray::fromStdString(plan.logFile) + "\n"
                                                  "result-file=" + QByteArray::fromStdString(plan.resultFile) + "\n"
                                                  "parent-pid=1\n"
                                                  "restart=restart_probe.exe\n"
                                                  "file=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef|1|../evil.dll\n"
                                                  "end\n"),
                                       error));
    QCOMPARE(runHelper({QStringLiteral("--plan"), tree.planFile, QStringLiteral("--no-restart")}), 2);
    QCOMPARE(zto_update_test::readFile(QDir(tree.install).filePath(QStringLiteral("a.dll"))),
             QByteArrayLiteral("old"));
    QVERIFY(!QFileInfo::exists(QDir(tree.temporary.path()).filePath(QStringLiteral("evil.dll"))));
}

void EngineTests::recoveryRestoresInterruptedTransaction()
{
    Tree tree;
    QString error;
    QVERIFY2(tree.initialize(error), qPrintable(error));
    QDir().mkpath(QDir(tree.transaction).filePath(QStringLiteral("backup")));
    QVERIFY(zto_update_test::writeFile(QDir(tree.transaction).filePath(QStringLiteral("backup/a.dll")),
                                       "original-bytes", error));
    QVERIFY(zto_update_test::writeFile(QDir(tree.install).filePath(QStringLiteral("a.dll")),
                                       "half-installed", error));
    PlanFile fileA;
    fileA.path = "a.dll";
    fileA.size = 14;
    fileA.sha256 = zto_update_test::sha256Hex("original-bytes").toStdString();
    auto plan = buildPlan(tree, {fileA}, {}, 4242);
    std::string planText;
    std::string planError;
    QVERIFY2(serializePlan(plan, planText, planError), planError.c_str());
    QVERIFY(zto_update_test::writeFile(QDir(tree.transaction).filePath(QStringLiteral("plan.txt")),
                                       QByteArray::fromStdString(planText), error));
    QVERIFY(zto_update_test::writeFile(QDir(tree.transaction).filePath(QStringLiteral("journal.txt")),
                                       "started\nbackup|a.dll\ninstalled|a.dll\n", error));
    std::string recoverError;
    QVERIFY2(recoverTransaction(tree.transaction.toStdString(), recoverError), recoverError.c_str());
    QCOMPARE(zto_update_test::readFile(QDir(tree.install).filePath(QStringLiteral("a.dll"))),
             QByteArrayLiteral("original-bytes"));
    QVERIFY(!QFileInfo::exists(tree.transaction));
}

void EngineTests::recoveryCommittedOnlyCleansUp()
{
    Tree tree;
    QString error;
    QVERIFY2(tree.initialize(error), qPrintable(error));
    QDir().mkpath(QDir(tree.transaction).filePath(QStringLiteral("backup")));
    QVERIFY(zto_update_test::writeFile(QDir(tree.transaction).filePath(QStringLiteral("backup/a.dll")),
                                       "original", error));
    QVERIFY(zto_update_test::writeFile(QDir(tree.install).filePath(QStringLiteral("a.dll")),
                                       "committed-new", error));
    PlanFile fileA;
    fileA.path = "a.dll";
    fileA.size = 8;
    fileA.sha256 = zto_update_test::sha256Hex("original").toStdString();
    auto plan = buildPlan(tree, {fileA}, {}, 4242);
    std::string planText;
    std::string planError;
    QVERIFY2(serializePlan(plan, planText, planError), planError.c_str());
    QVERIFY(zto_update_test::writeFile(QDir(tree.transaction).filePath(QStringLiteral("plan.txt")),
                                       QByteArray::fromStdString(planText), error));
    QVERIFY(zto_update_test::writeFile(QDir(tree.transaction).filePath(QStringLiteral("journal.txt")),
                                       "started\nbackup|a.dll\ninstalled|a.dll\nverified\ncommitted\n",
                                       error));
    std::string recoverError;
    QVERIFY2(recoverTransaction(tree.transaction.toStdString(), recoverError), recoverError.c_str());
    QCOMPARE(zto_update_test::readFile(QDir(tree.install).filePath(QStringLiteral("a.dll"))),
             QByteArrayLiteral("committed-new"));
    QVERIFY(!QFileInfo::exists(tree.transaction));
}

void EngineTests::detectsSelfReplacement()
{
    Tree tree;
    QString error;
    QVERIFY2(tree.initialize(error), qPrintable(error));
    PlanFile fileA;
    fileA.path = "z2_updater.exe";
    fileA.size = 1;
    fileA.sha256 = zto_update_test::sha256Hex("x").toStdString();
    const auto plan = buildPlan(tree, {fileA}, {}, 1);
    QVERIFY(planReplacesPath(plan,
                             QDir::toNativeSeparators(QDir(tree.install).filePath(QStringLiteral("z2_updater.exe"))).toStdString()));
    QVERIFY(!planReplacesPath(plan,
                              QDir::toNativeSeparators(QDir(tree.install).filePath(QStringLiteral("other.exe"))).toStdString()));
}

QTEST_GUILESS_MAIN(EngineTests)
#include "EngineTests.moc"
