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
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <ztprotocol/NetworkAddress.h>
#include <QFile>
#include <QJsonArray>
#include <QNetworkProxy>
#include <QDateTime>
#include <QFontDatabase>
#include <QtTest>
#include "PluginRuntime.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#endif
using namespace vusiif::ztgui::api;
using namespace vusiif::ztgui::plugins;
class VoicePluginTests : public QObject {
    Q_OBJECT
    static QJsonObject control(QTcpSocket &socket,QByteArray &buffer,int &serial,const QString &op,const QJsonObject &data={}) {
        const int id=++serial;
        socket.write(vusiif::ztgui::protocol::frame("voice-chat",QJsonDocument(QJsonObject{{"v",1},{"id",id},{"op",op},{"data",data}}).toJson(QJsonDocument::Compact)));
        socket.flush(); QElapsedTimer timer; timer.start();
        while(timer.elapsed()<5000) {
            buffer+=socket.readAll(); vusiif::ztgui::protocol::Packet packet;
            while(vusiif::ztgui::protocol::takeFrame(buffer,packet)==vusiif::ztgui::protocol::ReadResult::Packet) {
                const auto reply=QJsonDocument::fromJson(packet.payload).object();
                if(reply["id"].toInt()==id) return reply;
            }
            socket.waitForReadyRead(10); QCoreApplication::processEvents();
        }
        return {{"ok",false},{"error","control timeout"}};
    }
    static QObject *controller(QQuickItem *page) { return page->property("controller").value<QObject *>(); }
    static void click(QQuickWindow &window, QQuickItem *page, const char *name) {
        auto *button=page->findChild<QQuickItem *>(QString::fromLatin1(name)); QVERIFY(button);
        QTest::mouseClick(&window,Qt::LeftButton,{},button->mapToScene(QPointF(button->width()/2,button->height()/2)).toPoint());
    }
private slots:
    void liveDeviceCaptureAndLoopback() {
        const auto endpoint=vusiif::ztgui::protocol::serverEndpoint(qEnvironmentVariable("ZTGUI_LIVE_VOICE_HOST"));
        if(!endpoint.valid()) QSKIP("Set ZTGUI_LIVE_VOICE_HOST to an isolated server reachable over TCP and UDP");
        const int rounds=qBound(1,qEnvironmentVariableIntValue("ZTGUI_LIVE_AUDIO_ROUNDS"),100);
        QFile measurements(QCoreApplication::applicationDirPath()+"/live-audio-memory.csv"); QVERIFY(measurements.open(QIODevice::WriteOnly));
        QTextStream csv(&measurements); csv<<"round,privateBytes,workingSet,runtimeDirectories,runtimeHandles\n";
        for(int round=0;round<rounds;++round) {
        QTemporaryDir settings; QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path()); QSettings().setValue("voice-chat/muted",true);
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        PluginRegistry registry; PluginLoader loader; QQmlEngine engine; QQuickItem parent;
        const auto loaded=loader.loadFile(QString::fromUtf8(VOICE_PLUGIN),registry); QVERIFY2(loaded.ok(),qPrintable(loaded.message));
        QTRY_VERIFY_WITH_TIMEOUT(!registry.plugins().isEmpty(),15000);
        QTRY_COMPARE_WITH_TIMEOUT(registry.preparationState("voice-chat"),PreparationState::Ready,15000);
        auto page=registry.page("voice-chat/rooms",&engine,&parent); QVERIFY(page.result.ok()); auto *model=controller(page.page); QVERIFY(model);
        if(model->property("inputs").toList().size()<2 || model->property("outputs").toList().size()<2)
            QSKIP("Physical capture/playback devices are unavailable");
        model->setProperty("address",qEnvironmentVariable("ZTGUI_LIVE_VOICE_HOST")); model->setProperty("nickname","Physical audio test");
        model->setProperty("outputVolume",0); model->setProperty("inputVolume",100);
        QVERIFY(QMetaObject::invokeMethod(model,"toggleConnection")); QTRY_VERIFY(model->property("connected").toBool());
        QVERIFY(QMetaObject::invokeMethod(model,"createRoom",Q_ARG(QString,"Physical device "+QString::number(QDateTime::currentMSecsSinceEpoch()))));
        QTRY_COMPARE_WITH_TIMEOUT(model->property("members").toList().size(),1,5000);
        QTcpSocket observer; observer.setProxy(QNetworkProxy::NoProxy); observer.connectToHost(endpoint.host,endpoint.port); QVERIFY(observer.waitForConnected(5000));
        QByteArray controlBuffer; int serial=0;
        const auto hello=control(observer,controlBuffer,serial,"hello",{{"nickname","Audio observer"}}); QVERIFY2(hello["ok"].toBool(),qPrintable(hello["error"].toString()));
        const auto session=quint32(hello["data"].toObject()["session"].toDouble()); const auto port=quint16(hello["data"].toObject()["udpPort"].toInt());
        const auto rooms=control(observer,controlBuffer,serial,"rooms"); QVERIFY(rooms["ok"].toBool()); quint32 room=0;
        for(const auto &value:rooms["data"].toArray()) if(value.toObject()["name"].toString()==model->property("currentRoom").toString()) room=quint32(value.toObject()["id"].toDouble());
        QVERIFY(room); const auto joined=control(observer,controlBuffer,serial,"join",{{"room",int(room)}}); QVERIFY(joined["ok"].toBool());
        const auto token=QByteArray::fromHex(joined["data"].toObject()["token"].toString().toLatin1()); QCOMPARE(token.size(),16);
        QUdpSocket udp; udp.setProxy(QNetworkProxy::NoProxy); QVERIFY(udp.bind(QHostAddress::AnyIPv4,0));
        QByteArray registration(30,'\0'); registration.replace(0,4,"ZTV1"); registration[4]=1; registration[5]=1; registration.replace(6,16,token);
        qToBigEndian(session,reinterpret_cast<uchar *>(registration.data()+22));
        QVERIFY(udp.writeDatagram(vusiif::ztgui::protocol::encode("voice-chat",registration),observer.peerAddress(),port)>0);
        QTRY_VERIFY_WITH_TIMEOUT(udp.hasPendingDatagrams(),3000);
        while(udp.hasPendingDatagrams()) udp.receiveDatagram();
        QTRY_COMPARE(model->property("members").toList().size(),2);
        QVERIFY(QMetaObject::invokeMethod(model,"setMemberVolume",Q_ARG(quint32,session),Q_ARG(int,150)));
        bool memberGain=false; for(const auto &member:model->property("members").toList()) {
            const auto row=member.toMap(); if(row["id"].toUInt()==session) memberGain=row["volume"].toInt()==150;
        }
        QVERIFY(memberGain);
        model->setProperty("muted",false);
        int captured=0; quint32 sequence=1; QElapsedTimer timer; timer.start();
        while(timer.elapsed()<4000 && captured<10) {
            QTest::qWait(20);
            while(udp.hasPendingDatagrams()) {
                vusiif::ztgui::protocol::Packet packet;
                if(!vusiif::ztgui::protocol::decode(udp.receiveDatagram().data(),packet) || packet.payload.size()<=30 || quint8(packet.payload[5])!=3) continue;
                ++captured;
                // Send captured Opus back through the real peer path at zero total gain.
                // This starts/writes the physical sink without audible feedback.
                auto reply=registration; reply[5]=3; qToBigEndian(sequence++,reinterpret_cast<uchar *>(reply.data()+26));
                reply+=packet.payload.mid(30); udp.writeDatagram(vusiif::ztgui::protocol::encode("voice-chat",reply),observer.peerAddress(),port);
            }
        }
        model->setProperty("muted",true); QTest::qWait(100);
        QVERIFY2(captured>=5,qPrintable("No real microphone Opus frames; "+model->property("status").toString()));
        QVERIFY2(!model->property("status").toString().contains(QStringLiteral("失败")),qPrintable(model->property("status").toString()));
        qInfo()<<"Physical microphone frames captured and looped back:"<<captured;
        QVERIFY(QMetaObject::invokeMethod(model,"toggleConnection")); QTRY_VERIFY(!model->property("connected").toBool());
        QVERIFY(loader.unload("voice-chat",registry).ok()); QTRY_VERIFY(registry.plugins().isEmpty());
#ifdef Q_OS_WIN
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb=sizeof(memory); QVERIFY(K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),sizeof(memory)));
        const auto stats=vusiif::ztgui::package::runtimeStatistics(); QCOMPARE(stats["directories"].toInt(),1); QCOMPARE(stats["handles"].toInt(),6);
        csv<<round<<','<<qulonglong(memory.PrivateUsage)<<','<<qulonglong(memory.WorkingSetSize)<<','<<stats["directories"].toInt()<<','<<stats["handles"].toInt()<<'\n'; csv.flush();
#endif
        }
    }
    void initTestCase() {
#ifdef Q_OS_WIN
        QVERIFY(!GetModuleHandleW(L"Qt6Multimedia.dll"));
        QVERIFY(!GetModuleHandleW(L"avcodec-61.dll"));
#endif
    }
    void repeatedLoadsKeepOneRuntimeAndUnloadBusiness() {
        using namespace vusiif::ztgui::package;
        QTemporaryDir settings; QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
        QFile observations(qEnvironmentVariable("ZTGUI_RUNTIME_OBSERVATION_FILE",QCoreApplication::applicationDirPath()+"/runtime-memory.csv"));
        QVERIFY(observations.open(QIODevice::WriteOnly));
        QTextStream csv(&observations); csv<<"round,privateBytes,workingSet,threads,runtimeDirectories,runtimeHandles\n";
        QJsonObject warmed;
        for(int round=0;round<22;++round) {
            PluginRegistry registry; PluginLoader loader; QQmlEngine engine; QQuickItem parent;
            const auto result=loader.loadFile(QString::fromUtf8(VOICE_PLUGIN),registry);
            QVERIFY2(result.ok(),qPrintable(result.message));
            QTRY_VERIFY_WITH_TIMEOUT(!registry.plugins().isEmpty(),15000);
            QTRY_COMPARE_WITH_TIMEOUT(registry.preparationState("voice-chat"),PreparationState::Ready,15000);
            auto page=registry.page("voice-chat/rooms",&engine,&parent); QVERIFY2(page.result.ok(),qPrintable(page.result.message));
            auto *model=controller(page.page); QVERIFY(model);
            QVERIFY(model->property("inputs").isValid()); QVERIFY(model->property("outputs").isValid());
            if(round==0) qInfo()<<"Available audio devices:"<<model->property("inputs").toList().size()-1<<"inputs,"<<model->property("outputs").toList().size()-1<<"outputs";
            model->setProperty("inputVolume",125); model->setProperty("outputVolume",65); model->setProperty("muted",true);
            QCOMPARE(model->property("inputVolume").toInt(),125); QCOMPARE(model->property("outputVolume").toInt(),65);
#ifdef Q_OS_WIN
            const auto multimedia=GetModuleHandleW(L"Qt6Multimedia.dll"); QVERIFY(multimedia);
            wchar_t modulePath[32768]; const auto length=GetModuleFileNameW(multimedia,modulePath,32768);
            const auto source=QString::fromWCharArray(modulePath,length);
            if(round==0) qInfo()<<"Private Multimedia source:"<<source;
            QVERIFY2(source.contains("/runtime-",Qt::CaseInsensitive) || source.contains("\\runtime-",Qt::CaseInsensitive),qPrintable(source));
            const auto backend=GetModuleHandleW(L"ffmpegmediaplugin.dll"); QVERIFY(backend);
            GetModuleFileNameW(backend,modulePath,32768);
            QVERIFY(QString::fromWCharArray(modulePath).contains("runtime/qt-plugins/multimedia") ||
                    QString::fromWCharArray(modulePath).contains("runtime\\qt-plugins\\multimedia"));
#endif
            QPointer<QObject> alive=model; QSignalSpy finished(&loader,&PluginLoader::unloadFinished);
            QVERIFY(loader.unload("voice-chat",registry).ok()); QTRY_COMPARE(finished.size(),1);
            QVERIFY2(finished.first()[1].toBool(),qPrintable(finished.first()[2].toString())); QVERIFY(alive.isNull());
            QVERIFY(registry.plugins().isEmpty());
            const auto stats=runtimeStatistics();
            if(round==1) warmed=stats;
            if(round>1) { QCOMPARE(stats["directories"],warmed["directories"]); QCOMPARE(stats["handles"],warmed["handles"]); }
#ifdef Q_OS_WIN
            // The business DLL must be gone even while Qt retains application-lifetime backends.
            const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,GetCurrentProcessId());
            QVERIFY(snapshot!=INVALID_HANDLE_VALUE); MODULEENTRY32W module{}; module.dwSize=sizeof(module);
            if(Module32FirstW(snapshot,&module)) do {
                const auto path=QString::fromWCharArray(module.szExePath);
                QVERIFY2(!(path.contains("\\load-") && path.endsWith("\\plugin.dll")),qPrintable(path));
            } while(Module32NextW(snapshot,&module)); CloseHandle(snapshot);
            PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb=sizeof(memory); QVERIFY(K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),sizeof(memory)));
            int threads=0; const auto threadSnapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
            THREADENTRY32 thread{}; thread.dwSize=sizeof(thread);
            if(Thread32First(threadSnapshot,&thread)) do { if(thread.th32OwnerProcessID==GetCurrentProcessId()) ++threads; } while(Thread32Next(threadSnapshot,&thread));
            CloseHandle(threadSnapshot);
            csv<<round<<','<<qulonglong(memory.PrivateUsage)<<','<<qulonglong(memory.WorkingSetSize)<<','<<threads<<','<<stats["directories"].toInt()<<','<<stats["handles"].toInt()<<'\n'; csv.flush();
#endif
        }
    }
    void panelOnlyConnectsOnClick() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QCoreApplication::setOrganizationName("ZTGUI-Test"); QCoreApplication::setApplicationName("VoicePlugin");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        const auto library=QString::fromUtf8(VOICE_PLUGIN);
        QVERIFY(QFile::copy(library,directory.filePath(QFileInfo(library).fileName())));
        QQmlEngine engine; QQuickWindow window; window.resize(920,780); window.show();
        auto registry=std::make_unique<PluginRegistry>(); auto loader=std::make_unique<PluginLoader>();
        QCOMPARE(loader->scan(directory.path(),*registry).size(),1);
        QTRY_VERIFY_WITH_TIMEOUT(!registry->plugins().isEmpty(),15000);
        QTRY_COMPARE_WITH_TIMEOUT(registry->preparationState("voice-chat"),PreparationState::Ready,15000);
        auto page=registry->page("voice-chat/rooms",&engine,window.contentItem()); QVERIFY2(page.result.ok(),qPrintable(page.result.message));
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
        QVERIFY(window.grabWindow().save(QCoreApplication::applicationDirPath()+"/voice-qml.png"));
        click(window,item,"voiceConnect"); QTRY_VERIFY(server.hasPendingConnections());
        auto *socket=server.nextPendingConnection(); QTRY_VERIFY(socket->bytesAvailable()>=4);
        const auto prefix=socket->peek(4); const auto size=qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(prefix.constData()));
        QTRY_VERIFY(socket->bytesAvailable()>=4+size);
        QByteArray input=socket->readAll(); vusiif::ztgui::protocol::Packet packet;
        QCOMPARE(vusiif::ztgui::protocol::takeFrame(input,packet),vusiif::ztgui::protocol::ReadResult::Packet);
        QCOMPARE(packet.plugin,QByteArray("voice-chat"));
        const auto hello=QJsonDocument::fromJson(packet.payload).object(); QCOMPARE(hello["op"].toString(),QString("hello"));
        const QJsonObject response{{"v",1},{"id",hello["id"]},{"ok",true},{"data",QJsonObject{{"session",1},{"udpPort",18100},{"rooms",QJsonArray{}}}}};
        socket->write(vusiif::ztgui::protocol::frame("voice-chat",QJsonDocument(response).toJson(QJsonDocument::Compact)));
        QTRY_VERIFY(model->property("connected").toBool());
        QTRY_VERIFY(item->findChild<QQuickItem *>("voiceCreateRoom")->isEnabled());
        model->setProperty("muted",true); model->setProperty("deafened",true); model->setProperty("volume",35);
        QCOMPARE(QSettings().value("voice-chat/volume").toInt(),35);
        click(window,item,"voiceConnect"); QTRY_VERIFY(!model->property("connected").toBool());
        QPointer<QObject> alive=model; QVERIFY(loader->unload("voice-chat",*registry).ok()); QTRY_VERIFY(alive.isNull());
        // Reloading after unloading must not retain a compiled page/controller from the DLL.
        QVERIFY(loader->loadFile(directory.filePath(QFileInfo(library).fileName()),*registry).ok());
        QTRY_VERIFY_WITH_TIMEOUT(!registry->plugins().isEmpty(),15000);
        QTRY_COMPARE_WITH_TIMEOUT(registry->preparationState("voice-chat"),PreparationState::Ready,15000);
        page=registry->page("voice-chat/rooms",&engine,window.contentItem()); QVERIFY(page.result.ok());
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
        auto firstRegistry=std::make_unique<PluginRegistry>(),secondRegistry=std::make_unique<PluginRegistry>();
        auto loader=std::make_unique<PluginLoader>(),secondLoader=std::make_unique<PluginLoader>();
        loader->scan(directory.path(),*firstRegistry); secondLoader->scan(directory.path(),*secondRegistry);
        QTRY_VERIFY_WITH_TIMEOUT(!firstRegistry->plugins().isEmpty(),15000);
        QTRY_COMPARE_WITH_TIMEOUT(firstRegistry->preparationState("voice-chat"),PreparationState::Ready,15000);
        QTRY_VERIFY_WITH_TIMEOUT(!secondRegistry->plugins().isEmpty(),15000);
        QTRY_COMPARE_WITH_TIMEOUT(secondRegistry->preparationState("voice-chat"),PreparationState::Ready,15000);
        auto *firstPage=firstRegistry->page("voice-chat/rooms",&firstEngine,&firstParent).page;
        auto *secondPage=secondRegistry->page("voice-chat/rooms",&secondEngine,&secondParent).page;
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
        loader.reset(); secondLoader.reset(); firstRegistry.reset(); secondRegistry.reset(); QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
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
