#include "VoiceController.h"
#include "AudioDeviceCache.h"
#include <ztgui/PluginPage.h>
#include <ztprotocol/NetworkAddress.h>
#include <ztprotocol/WireProtocol.h>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QSettings>
#include <QDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFontDatabase>
#include <QtTest>
using namespace vusiif::ztgui;

class VoiceUiTests : public QObject {
    Q_OBJECT
private slots:
    void independentInputAndOutputPreferences() {
        QTemporaryDir directory;
        QCoreApplication::setOrganizationName("ZTGUI-Voice-UI"); QCoreApplication::setApplicationName("VoiceUI");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        QSettings().setValue("voice-chat/volume",80);
        { voice_chat::VoiceController c(nullptr,std::make_shared<voice_chat::AudioDeviceCache>());
          QCOMPARE(c.volume(),80); QCOMPARE(c.inputVolume(),100); c.setInputVolume(150); c.setVolume(200);
          QCOMPARE(c.inputVolume(),150); QCOMPARE(c.volume(),200); c.setMemberVolume(1,0); QCOMPARE(c.memberVolume(1),100); }
        { voice_chat::VoiceController c(nullptr,std::make_shared<voice_chat::AudioDeviceCache>());
          QCOMPARE(c.inputVolume(),150); QCOMPARE(c.volume(),200); c.setInputVolume(900); QCOMPARE(c.inputVolume(),200); c.setVolume(-1); QCOMPARE(c.volume(),0); }
    }
    void endpointParsing() {
        QCOMPARE(protocol::serverEndpoint("example.com").port, quint16(18100));
        QCOMPARE(protocol::serverEndpoint("127.0.0.1:19000").port, quint16(19000));
        QCOMPARE(protocol::serverEndpoint("[::1]:18101").host, QString("::1"));
        QCOMPARE(protocol::serverEndpoint("::1").port, quint16(18100));
        for (const auto &address : {"host:0","host:65536","host:abc","host:","host/path","user@host","host?query",""})
            QVERIFY2(!protocol::serverEndpoint(address).valid(), address);
    }
    void pageAndAudioPopup_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false; QTest::newRow("dark") << true;
    }
    void pageAndAudioPopup() {
        QFETCH(bool,dark);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QCoreApplication::setOrganizationName("ZTGUI-Voice-UI"); QCoreApplication::setApplicationName("VoiceUI");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        QSettings().setValue("voice-chat/port",19001); // Obsolete preference must not override the default.
        voice_chat::VoiceController controller(nullptr,std::make_shared<voice_chat::AudioDeviceCache>());
        QQmlEngine engine; QStringList warnings;
        connect(&engine,&QQmlEngine::warnings,this,[&](const QList<QQmlError> &errors){ for(const auto &error:errors) warnings.append(error.toString()); });
        QQmlComponent shell(&engine);
        shell.setData(dark ? QByteArray("import QtQuick; import QtQuick.Controls.Basic; ApplicationWindow { width: 920; height: 720; visible: true; palette.window: '#202830'; palette.windowText: '#e1ebf3'; palette.base: '#1d252e' }")
                           : QByteArray("import QtQuick; import QtQuick.Controls.Basic; ApplicationWindow { width: 920; height: 720; visible: true; palette.window: '#f4f7fa'; palette.windowText: '#20394c'; palette.base: '#ffffff' }"),QUrl());
        std::unique_ptr<QQuickWindow> window(qobject_cast<QQuickWindow *>(shell.create())); QVERIFY2(window,qPrintable(shell.errorString()));
        auto *page=api::createQmlPage(&engine,window->contentItem(),QUrl::fromLocalFile(QString::fromUtf8(VOICE_PAGE)),{{"controller",QVariant::fromValue(&controller)}});
        QVERIFY(page); page->setWidth(920); page->setHeight(720);
        QTest::qWait(220);
        QVERIFY(!page->findChild<QObject *>("voiceServerPort"));
        auto *gear=page->findChild<QQuickItem *>("voiceAudioSettings"); QVERIFY(gear);
        QTest::mouseClick(window.get(),Qt::LeftButton,{},gear->mapToScene(QPointF(gear->width()/2,gear->height()/2)).toPoint());
        auto *popup=page->findChild<QObject *>("voiceAudioPopup"); QTRY_VERIFY(popup);
        QTRY_VERIFY(popup->property("visible").toBool()); QTest::qWait(220);
        QVERIFY(page->findChild<QObject *>("voiceInputDevice"));
        QVERIFY(QDir().mkpath("screenshots"));
        QVERIFY(window->grabWindow().save(dark?"screenshots/voice-audio-dark.png":"screenshots/voice-audio-light.png"));
        QTest::keyClick(window.get(),Qt::Key_Escape); QTRY_VERIFY(!popup->property("visible").toBool());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost,18100));
        controller.setAddress("127.0.0.1"); controller.setNickname("Test"); controller.toggleConnection();
        QTRY_VERIFY(server.hasPendingConnections()); auto *socket=server.nextPendingConnection();
        QTRY_VERIFY(socket->bytesAvailable()>=4);
        const auto prefix=socket->peek(4); const auto size=qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(prefix.constData()));
        QTRY_VERIFY(socket->bytesAvailable()>=4+size);
        QByteArray input=socket->readAll(); protocol::Packet packet;
        QCOMPARE(protocol::takeFrame(input,packet),protocol::ReadResult::Packet);
        const auto hello=QJsonDocument::fromJson(packet.payload).object();
        const QJsonArray rooms{QJsonObject{{"id",1},{"name","Room A"},{"members",1},{"capacity",10}},QJsonObject{{"id",2},{"name","Room B"},{"members",2},{"capacity",10}}};
        const QJsonObject reply{{"v",1},{"id",hello["id"]},{"ok",true},{"data",QJsonObject{{"session",1},{"udpPort",18100},{"rooms",rooms}}}};
        socket->write(protocol::frame("voice-chat",QJsonDocument(reply).toJson(QJsonDocument::Compact)));
        QTRY_VERIFY(controller.connected()); QTRY_COMPARE(controller.rooms().size(),2);
        page->setProperty("selectedRoom",1); QTest::qWait(220);
        QVERIFY(window->grabWindow().save(dark?"screenshots/voice-rooms-dark.png":"screenshots/voice-rooms-light.png"));
        controller.toggleConnection(); QTRY_VERIFY(!controller.connected());
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
    }
};
int main(int argc,char **argv) {
    qputenv("QT_QUICK_BACKEND","software");
    QGuiApplication app(argc,argv); QQuickStyle::setStyle("Basic");
#ifdef Q_OS_WIN
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR","C:/Windows")+"/Fonts/msyh.ttc"); app.setFont(QFont("Microsoft YaHei",10));
#endif
    VoiceUiTests tests; return QTest::qExec(&tests,argc,argv);
}
#include "VoiceUiTests.moc"
