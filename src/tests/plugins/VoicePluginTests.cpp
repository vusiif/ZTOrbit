#include "../../client/main/plugins/PluginLoader.h"
#include "../../client/main/plugins/PluginRegistry.h"
#include <ztprotocol/WireProtocol.h>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFile>
#include <QJsonArray>
#include <QNetworkProxy>
#include <QDateTime>
#include <QFontDatabase>
#include <QtTest>
using namespace vusiif::ztgui::api;
using namespace vusiif::ztgui::plugins;
class VoicePluginTests : public QObject {
    Q_OBJECT
    static QObject *controller(QQuickItem *page) { return page->property("controller").value<QObject *>(); }
    static void click(QQuickWindow &window, QQuickItem *page, const char *name) {
        auto *button=page->findChild<QQuickItem *>(QString::fromLatin1(name)); QVERIFY(button);
        QTest::mouseClick(&window,Qt::LeftButton,{},button->mapToScene(QPointF(button->width()/2,button->height()/2)).toPoint());
    }
private slots:
    void panelOnlyConnectsOnClick() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QCoreApplication::setOrganizationName("ZTGUI-Test"); QCoreApplication::setApplicationName("VoicePlugin");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        const auto library=QString::fromUtf8(VOICE_PLUGIN);
        QVERIFY(QFile::copy(library,directory.filePath(QFileInfo(library).fileName())));
        QQmlEngine engine; QQuickWindow window; window.resize(920,780); window.show();
        auto registry=std::make_unique<PluginRegistry>(); auto loader=std::make_unique<PluginLoader>();
        QCOMPARE(loader->scan(directory.path(),*registry).size(),1);
        QTRY_COMPARE_WITH_TIMEOUT(registry->preparationState("org.ztgui.voice-chat"),PreparationState::Ready,15000);
        auto page=registry->page("org.ztgui.voice-chat/rooms",&engine,window.contentItem()); QVERIFY2(page.result.ok(),qPrintable(page.result.message));
        auto *item=page.page; item->setWidth(920); item->setHeight(780);
        auto *model=controller(item); QVERIFY(model);
        QVERIFY(!item->findChild<QQuickItem *>("voiceServerPort"));
        QVERIFY(item->findChild<QQuickItem *>("voiceAudioSettings"));
        click(window,item,"voiceAudioSettings");
        QTRY_VERIFY(item->findChild<QObject *>("voiceAudioPopup"));
        QTest::qWait(200);
        QVERIFY(item->findChild<QObject *>("voiceAudioPopup")->property("visible").toBool());
        QTest::keyClick(&window,Qt::Key_Escape);
        QTRY_VERIFY(!item->findChild<QObject *>("voiceAudioPopup")->property("visible").toBool());
        QVERIFY(model->property("address").toString().isEmpty());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost,0));
        model->setProperty("address",QString("127.0.0.1:%1").arg(server.serverPort())); model->setProperty("nickname","Test");
        QTest::qWait(100); QVERIFY(!server.hasPendingConnections());
        QVERIFY(window.grabWindow().save("voice-qml.png"));
        click(window,item,"voiceConnect"); QTRY_VERIFY(server.hasPendingConnections());
        auto *socket=server.nextPendingConnection(); QTRY_VERIFY(socket->bytesAvailable()>=4);
        const auto prefix=socket->peek(4); const auto size=qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(prefix.constData()));
        QTRY_VERIFY(socket->bytesAvailable()>=4+size);
        QByteArray input=socket->readAll(); vusiif::ztgui::protocol::Packet packet;
        QCOMPARE(vusiif::ztgui::protocol::takeFrame(input,packet),vusiif::ztgui::protocol::ReadResult::Packet);
        QCOMPARE(packet.plugin,QByteArray("org.ztgui.voice-chat"));
        const auto hello=QJsonDocument::fromJson(packet.payload).object(); QCOMPARE(hello["op"].toString(),QString("hello"));
        const QJsonObject response{{"v",1},{"id",hello["id"]},{"ok",true},{"data",QJsonObject{{"session",1},{"udpPort",18100},{"rooms",QJsonArray{}}}}};
        socket->write(vusiif::ztgui::protocol::frame("org.ztgui.voice-chat",QJsonDocument(response).toJson(QJsonDocument::Compact)));
        QTRY_VERIFY(model->property("connected").toBool());
        QTRY_VERIFY(item->findChild<QQuickItem *>("voiceCreateRoom")->isEnabled());
        model->setProperty("muted",true); model->setProperty("deafened",true); model->setProperty("volume",35);
        QCOMPARE(QSettings().value("voice-chat/volume").toInt(),35);
        click(window,item,"voiceConnect"); QTRY_VERIFY(!model->property("connected").toBool());
        QPointer<QObject> alive=model; QVERIFY(loader->unload("org.ztgui.voice-chat",*registry).ok()); QVERIFY(alive.isNull());
        // Reloading after unloading must not retain a compiled page/controller from the DLL.
        loader->scan(directory.path(),*registry);
        QTRY_COMPARE_WITH_TIMEOUT(registry->preparationState("org.ztgui.voice-chat"),PreparationState::Ready,15000);
        page=registry->page("org.ztgui.voice-chat/rooms",&engine,window.contentItem()); QVERIFY(page.result.ok());
        QCOMPARE(controller(page.page)->property("volume").toInt(),35);
        loader.reset(); registry.reset();
    }
    void liveRoomLifecycle() {
        const auto host=qEnvironmentVariable("ZTGUI_LIVE_VOICE_HOST");
        if (host.isEmpty()) QSKIP("Set ZTGUI_LIVE_VOICE_HOST to test the deployed server");
        QTemporaryDir directory; QVERIFY(directory.isValid()); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        QSettings().setValue("voice-chat/muted",true);
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy,"127.0.0.1",1));
        const auto library=QString::fromUtf8(VOICE_PLUGIN); QVERIFY(QFile::copy(library,directory.filePath(QFileInfo(library).fileName())));
        QQmlEngine firstEngine,secondEngine; QQuickItem firstParent,secondParent;
        auto firstRegistry=std::make_unique<PluginRegistry>(),secondRegistry=std::make_unique<PluginRegistry>(); auto loader=std::make_unique<PluginLoader>();
        loader->scan(directory.path(),*firstRegistry); loader->scan(directory.path(),*secondRegistry);
        QTRY_COMPARE_WITH_TIMEOUT(firstRegistry->preparationState("org.ztgui.voice-chat"),PreparationState::Ready,15000);
        QTRY_COMPARE_WITH_TIMEOUT(secondRegistry->preparationState("org.ztgui.voice-chat"),PreparationState::Ready,15000);
        auto *firstPage=firstRegistry->page("org.ztgui.voice-chat/rooms",&firstEngine,&firstParent).page;
        auto *secondPage=secondRegistry->page("org.ztgui.voice-chat/rooms",&secondEngine,&secondParent).page;
        QVERIFY(firstPage); QVERIFY(secondPage); auto *first=controller(firstPage),*second=controller(secondPage);
        for (auto *model : {first,second}) {
            model->setProperty("address",host); model->setProperty("nickname",model==first?"PluginTest-A":"PluginTest-B");
            QVERIFY(QMetaObject::invokeMethod(model,"toggleConnection")); QTRY_VERIFY_WITH_TIMEOUT(model->property("connected").toBool(),5000);
        }
        const auto roomName="Plugin test "+QString::number(QDateTime::currentMSecsSinceEpoch());
        QVERIFY(QMetaObject::invokeMethod(first,"createRoom",Q_ARG(QString,roomName)));
        QTRY_COMPARE_WITH_TIMEOUT(first->property("members").toList().size(),1,5000);
        QTRY_VERIFY_WITH_TIMEOUT(!second->property("rooms").toList().isEmpty(),5000);
        quint32 roomId=0; for (const auto &value : second->property("rooms").toList()) if (value.toMap()["name"].toString()==roomName) roomId=value.toMap()["id"].toUInt();
        QVERIFY(roomId); QVERIFY(QMetaObject::invokeMethod(second,"joinRoom",Q_ARG(quint32,roomId)));
        QTRY_COMPARE_WITH_TIMEOUT(first->property("members").toList().size(),2,5000); QTRY_COMPARE_WITH_TIMEOUT(second->property("members").toList().size(),2,5000);
        QVERIFY(QMetaObject::invokeMethod(second,"leaveRoom")); QTRY_COMPARE_WITH_TIMEOUT(first->property("members").toList().size(),1,5000);
        loader.reset(); firstRegistry.reset(); secondRegistry.reset(); QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    }
};
int main(int argc,char **argv) {
    qputenv("QT_QUICK_BACKEND","software");
#ifdef Q_OS_WIN
    qputenv("QT_QPA_FONTDIR",(qEnvironmentVariable("WINDIR","C:/Windows")+"/Fonts").toLocal8Bit());
#endif
    QGuiApplication app(argc,argv); QQuickStyle::setStyle("Basic");
#ifdef Q_OS_WIN
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR","C:/Windows")+"/Fonts/msyh.ttc"); app.setFont(QFont("Microsoft YaHei",10));
#endif
    VoicePluginTests tests; return QTest::qExec(&tests,argc,argv);
}
#include "VoicePluginTests.moc"
