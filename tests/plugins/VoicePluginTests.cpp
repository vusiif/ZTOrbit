#include "../../src/main/plugins/PluginLoader.h"
#include "../../src/main/plugins/PluginRegistry.h"
#include "../../src/plugins/voice-chat/Protocol.h"
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QListWidget>
#include <QLabel>
#include <QTemporaryDir>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFile>
#include <QJsonArray>
#include <QCheckBox>
#include <QFontDatabase>
#include <QNetworkProxy>
#include <QDateTime>
#include "../../src/main/AppStyle.h"
#include <QtTest>
using namespace vusiif::ztgui::api;
using namespace vusiif::ztgui::util;
using namespace vusiif::ztgui::plugins;
using namespace vusiif::ztgui::ui;
class VoicePluginTests : public QObject {
    Q_OBJECT
private slots:
    void panelOnlyConnectsOnClick() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QCoreApplication::setOrganizationName("ZTGUI-Test"); QCoreApplication::setApplicationName("VoicePlugin");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        const auto fontPath = qEnvironmentVariable("ZTGUI_TEST_FONT");
        if (!fontPath.isEmpty()) QFontDatabase::addApplicationFont(fontPath);
        vusiif::ztgui::ui::applyAppStyle(*qApp);
        const auto library = QString::fromUtf8(VOICE_PLUGIN);
        QVERIFY(QFile::copy(library, directory.filePath(QFileInfo(library).fileName())));
        auto loader = std::make_unique<PluginLoader>();
        auto registry = std::make_unique<PluginRegistry>();
        QCOMPARE(loader->scan(directory.path(), *registry).size(), 1);
        QCOMPARE(registry->plugins().size(), 1);
        QCOMPARE(registry->preparationState("org.ztgui.voice-chat"), PreparationState::Preparing);
        QTRY_COMPARE_WITH_TIMEOUT(registry->preparationState("org.ztgui.voice-chat"), PreparationState::Ready, 15000);
        QWidget parent; parent.resize(920, 780);
        auto page = registry->page("org.ztgui.voice-chat/rooms", &parent);
        QVERIFY(page.result.ok()); auto *widget = page.page; widget->resize(920, 780); widget->show();
        auto *address = widget->findChild<QLineEdit *>("voiceServerAddress");
        auto *port = widget->findChild<QSpinBox *>("voiceServerPort");
        auto *name = widget->findChild<QLineEdit *>("voiceNickname");
        auto *button = widget->findChild<QPushButton *>("voiceConnect");
        auto *status = widget->findChild<QLabel *>("voiceStatus");
        QVERIFY(address); QVERIFY(port); QVERIFY(name); QVERIFY(button); QVERIFY(status);
        QVERIFY(address->text().isEmpty()); QCOMPARE(port->value(), 18100);
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        address->setText("127.0.0.1"); port->setValue(server.serverPort()); name->setText("Test");
        QTest::qWait(100); QVERIFY(!server.hasPendingConnections());
        const auto screenshot = qEnvironmentVariable("ZTGUI_VOICE_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(widget->grab().save(screenshot));
        QTest::mouseClick(button, Qt::LeftButton);
        QTRY_VERIFY(server.hasPendingConnections()); auto *socket = server.nextPendingConnection();
        QTRY_VERIFY(socket->canReadLine());
        const auto hello = QJsonDocument::fromJson(socket->readLine()).object();
        QCOMPARE(hello.value("op").toString(), QStringLiteral("hello"));
        socket->write(vusiif::ztgui::voice_chat::jsonLine({{"v", 1}, {"id", hello.value("id")}, {"ok", true},
            {"data", QJsonObject{{"session", 1}, {"udpPort", 18100}, {"rooms", QJsonArray{}}}}}));
        QTRY_VERIFY(widget->findChild<QPushButton *>("voiceCreateRoom")->isEnabled());
        QTest::mouseClick(button, Qt::LeftButton);
        QTRY_COMPARE(button->text(), QStringLiteral("连接"));
        registry.reset(); loader.reset();
    }
    void liveRoomLifecycle() {
        const auto host = qEnvironmentVariable("ZTGUI_LIVE_VOICE_HOST");
        if (host.isEmpty()) QSKIP("Set ZTGUI_LIVE_VOICE_HOST to test the deployed server");
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        QSettings settings; settings.setValue("voice-chat/muted", true);
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 1));
        const auto library = QString::fromUtf8(VOICE_PLUGIN);
        QVERIFY(QFile::copy(library, directory.filePath(QFileInfo(library).fileName())));
        auto loader = std::make_unique<PluginLoader>();
        auto firstRegistry = std::make_unique<PluginRegistry>(), secondRegistry = std::make_unique<PluginRegistry>();
        loader->scan(directory.path(), *firstRegistry); loader->scan(directory.path(), *secondRegistry);
        QTRY_COMPARE_WITH_TIMEOUT(firstRegistry->preparationState("org.ztgui.voice-chat"), PreparationState::Ready, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(secondRegistry->preparationState("org.ztgui.voice-chat"), PreparationState::Ready, 15000);
        QWidget firstParent, secondParent;
        auto *first = firstRegistry->page("org.ztgui.voice-chat/rooms", &firstParent).page;
        auto *second = secondRegistry->page("org.ztgui.voice-chat/rooms", &secondParent).page;
        QVERIFY(first); QVERIFY(second);
        for (auto *page : {first, second}) {
            page->findChild<QLineEdit *>("voiceServerAddress")->setText(host);
            page->findChild<QSpinBox *>("voiceServerPort")->setValue(18100);
            page->findChild<QLineEdit *>("voiceNickname")->setText(page == first ? "PluginTest-A" : "PluginTest-B");
            page->findChild<QPushButton *>("voiceConnect")->click();
            QTRY_VERIFY2_WITH_TIMEOUT(page->findChild<QPushButton *>("voiceCreateRoom")->isEnabled(),
                qPrintable(page->findChild<QLabel *>("voiceStatus")->text()), 5000);
        }
        const auto roomName = "Plugin test " + QString::number(QDateTime::currentMSecsSinceEpoch());
        first->findChild<QLineEdit *>("voiceRoomName")->setText(roomName);
        first->findChild<QPushButton *>("voiceCreateRoom")->click();
        QTRY_COMPARE_WITH_TIMEOUT(first->findChild<QListWidget *>("voiceMembers")->count(), 1, 5000);
        auto *roomList = second->findChild<QListWidget *>("voiceRooms");
        QTRY_VERIFY_WITH_TIMEOUT(roomList->count() > 0, 5000);
        for (int row = 0; row < roomList->count(); ++row)
            if (roomList->item(row)->text().startsWith(roomName)) roomList->setCurrentRow(row);
        QVERIFY(roomList->currentItem());
        second->findChild<QPushButton *>("voiceJoinRoom")->click();
        QTRY_COMPARE_WITH_TIMEOUT(first->findChild<QListWidget *>("voiceMembers")->count(), 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(second->findChild<QListWidget *>("voiceMembers")->count(), 2, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(second->findChild<QLabel *>("voiceStatus")->text().contains(QStringLiteral("就绪")), 5000);
        second->findChild<QPushButton *>("voiceLeaveRoom")->click();
        QTRY_COMPARE_WITH_TIMEOUT(first->findChild<QListWidget *>("voiceMembers")->count(), 1, 5000);
        firstRegistry.reset(); secondRegistry.reset(); loader.reset();
    }
};
QTEST_MAIN(VoicePluginTests)
#include "VoicePluginTests.moc"
