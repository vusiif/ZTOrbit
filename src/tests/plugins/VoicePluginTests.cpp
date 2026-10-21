#include "../../client/main/plugins/PluginLoader.h"
#include "../../client/main/plugins/PluginRegistry.h"
#include <ztprotocol/WireProtocol.h>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <ztprotocol/NetworkAddress.h>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QDateTime>
#include <QFontDatabase>
#include <QtTest>
#include "PluginRuntime.h"
#include <QCryptographicHash>
#include <ztmedia/MediaSession.h>
#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#endif
using namespace vusiif::ztgui::api;
using namespace vusiif::ztgui::plugins;
class VoicePluginTests : public QObject {
    Q_OBJECT
    QTemporaryDir installed;
    QString voicePath;
    static QJsonObject voiceConfig() {
        QFile file(QDir(QCoreApplication::applicationDirPath()).filePath("plugins-data/id-voice-chat/config.json"));
        if (!file.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }
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
    void oldMediaPackageWithoutClipServiceIsRejected() {
        using namespace vusiif::ztgui;
        const auto media = package::inspect(MEDIA_PLUGIN);
        QVERIFY2(media.ok(), qPrintable(media.error));
        QVERIFY(media.info["provides"].toArray().size() > 0);
        std::unique_ptr<QTemporaryDir> extracted;
        std::unique_ptr<QLockFile> extractedLock;
        QString entry;
        QVERIFY2(package::extract(media, extracted, entry, extractedLock).isEmpty(), "extract media package");
        QJsonArray files;
        for (const auto &value : media.info["files"].toArray()) {
            auto item = value.toObject();
            item["source"] = QDir(extracted->path()).filePath(item["path"].toString());
            files.append(item);
        }
        QJsonArray provides;
        for (const auto &value : media.info["provides"].toArray()) {
            if (value.toObject()["service"].toString() != QStringLiteral("media.clip"))
                provides.append(value);
        }
        QCOMPARE(provides.size(), media.info["provides"].toArray().size() - 1);
        const QJsonObject manifest{{"files", files}, {"runtime", media.info["runtime"]},
            {"requiresHost", media.info["requiresHost"]},
            {"execution", media.info["execution"]},
            {"dependencies", media.info["dependencies"]},
            {"provides", provides}};
        QTemporaryDir install;
        QVERIFY(install.isValid());
        const auto oldMedia = install.filePath("media.ztplugin");
        const auto error = package::writeBundle(manifest, oldMedia, media.info);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFile::copy(VOICE_PLUGIN, install.filePath("voice.ztplugin")));
        PluginRegistry registry;
        PluginLoader loader;
        QSignalSpy finished(&loader, &PluginLoader::loadFinished);
        QVERIFY(loader.loadFile(install.filePath("voice.ztplugin"), registry).ok());
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 15000);
        QVERIFY(!finished.first()[1].toBool());
        const auto message = finished.first()[2].toString();
        QVERIFY2(message.contains(QStringLiteral("media.clip")), qPrintable(message));
        QVERIFY(registry.plugins().isEmpty());
        loader.requestShutdown();
        QTRY_VERIFY_WITH_TIMEOUT(loader.shutdownComplete(), 10000);
        extractedLock.reset();
        package::discardCache(extracted);
    }
    void mediaWorkerUsesPackagedCoreAndCrt() {
#ifdef Q_OS_WIN
        using namespace vusiif::ztgui;
        const auto pkg=package::inspect(MEDIA_PLUGIN); QVERIFY2(pkg.ok(),qPrintable(pkg.error));
        QCOMPARE(QFileInfo(pkg.info["entry"].toString()).path(),QString("runtime/bin"));
        std::unique_ptr<QTemporaryDir> cache; std::unique_ptr<QLockFile> lock; QString entry;
        QVERIFY(package::extract(pkg,cache,entry,lock).isEmpty());
        ServiceBroker broker; QSignalSpy failed(&broker,&ServiceBroker::failed); broker.start(entry,cache->path(),pkg.info);
        QTRY_VERIFY_WITH_TIMEOUT(broker.ready() || !failed.isEmpty(),80000); QVERIFY2(broker.ready(),qPrintable(broker.failure()));
        QMap<QString,QJsonObject> files;
        for(const auto &value:pkg.info["files"].toArray()) {
            const auto f=value.toObject(); if(f["role"]=="runtime-library" || f["role"]=="qt-plugin") files[QFileInfo(f["path"].toString()).fileName().toLower()]=f;
        }
        QSet<QString> mapped;
        const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,DWORD(broker.pid())); QVERIFY(snapshot!=INVALID_HANDLE_VALUE);
        MODULEENTRY32W module{}; module.dwSize=sizeof(module);
        if(Module32FirstW(snapshot,&module)) do {
            const auto path=QString::fromWCharArray(module.szExePath),name=QFileInfo(path).fileName().toLower();
            if(!files.contains(name)) continue;
            mapped.insert(name); const auto expected=cache->filePath(files[name]["path"].toString());
            QCOMPARE(QFileInfo(path).canonicalFilePath().toCaseFolded(),QFileInfo(expected).canonicalFilePath().toCaseFolded());
            QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCryptographicHash hash(QCryptographicHash::Sha256); QVERIFY(hash.addData(&file));
            QCOMPARE(QString::fromLatin1(hash.result().toHex()),files[name]["sha256"].toString());
        } while(Module32NextW(snapshot,&module));
        CloseHandle(snapshot);
        QVERIFY(mapped.contains("qt6core.dll")); QVERIFY(mapped.contains("msvcp140.dll")); QVERIFY(mapped.contains("vcruntime140.dll"));
        qInfo()<<"Worker application-local Qt/CRT and backend modules verified:"<<mapped;
        broker.stop(); QTRY_VERIFY_WITH_TIMEOUT(broker.exited(),8000); QVERIFY(!broker.forced());
        lock.reset(); package::discardCache(cache);
#endif
    }
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
        const auto loaded=loader.loadFile(voicePath,registry); QVERIFY2(loaded.ok(),qPrintable(loaded.message));
        QTRY_COMPARE_WITH_TIMEOUT(registry.plugins().size(),2,80000);
        QTRY_COMPARE_WITH_TIMEOUT(registry.preparationState("voice-chat"),PreparationState::Ready,15000);
        auto page=registry.page("voice-chat/rooms",&engine,&parent); QVERIFY(page.result.ok()); auto *model=controller(page.page); QVERIFY(model);
        QElapsedTimer deviceWait;deviceWait.start();
        while((model->property("inputs").toList().size()<2 || model->property("outputs").toList().size()<2)&&deviceWait.elapsed()<3000)QTest::qWait(10);
        if(model->property("inputs").toList().size()<2 || model->property("outputs").toList().size()<2)
            QSKIP("Physical capture/playback devices are unavailable");
        model->setProperty("address",qEnvironmentVariable("ZTGUI_LIVE_VOICE_HOST")); model->setProperty("nickname","Physical audio test");
        model->setProperty("volume",0); model->setProperty("inputVolume",100);
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
        const auto stats=vusiif::ztgui::package::runtimeStatistics(); QCOMPARE(stats["directories"].toInt(),0); QCOMPARE(stats["handles"].toInt(),0);
        csv<<round<<','<<qulonglong(memory.PrivateUsage)<<','<<qulonglong(memory.WorkingSetSize)<<','<<stats["directories"].toInt()<<','<<stats["handles"].toInt()<<'\n'; csv.flush();
#endif
        }
    }
    void initTestCase() {
        voicePath=installed.filePath("voice.ztplugin");
        QVERIFY(QFile::copy(QString::fromUtf8(VOICE_PLUGIN),voicePath));
        QVERIFY(QFile::copy(MEDIA_PLUGIN,installed.filePath("media.ztplugin")));
        // The production plugin uses its real per-user data root. Give these
        // integration checks a deterministic empty config and one-way import
        // marker so earlier runs cannot leak address/volume into the panel.
        const auto dataDir=QDir(QCoreApplication::applicationDirPath()).filePath("plugins-data/id-voice-chat");
        QDir(dataDir).removeRecursively();
        QVERIFY(QDir().mkpath(dataDir));
        QFile config(QDir(dataDir).filePath("config.json"));
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write(QJsonDocument(QJsonObject{{"format",1},{"address",""},{"nickname",""},
            {"volume",80},{"inputVolume",100},{"muted",false},{"deafened",false},
            {"input",""},{"output",""}}).toJson(QJsonDocument::Indented));
        config.close();
        const auto metadata=QDir(QCoreApplication::applicationDirPath()).filePath(".ztgui/plugins");
        QVERIFY(QDir().mkpath(metadata));
        QFile ownership(QDir(metadata).filePath("voice-chat.json"));
        QVERIFY(ownership.open(QIODevice::WriteOnly));
        ownership.write(QJsonDocument(QJsonObject{{"format",1},{"id","voice-chat"},
            {"names",QJsonArray{QStringLiteral("语音聊天")}},
            {"legacyImport",QJsonObject{{"completed",true}}}}).toJson(QJsonDocument::Indented));
        ownership.close();
#ifdef Q_OS_WIN
        QVERIFY(!GetModuleHandleW(L"Qt6Multimedia.dll"));
        QVERIFY(!GetModuleHandleW(L"avcodec-61.dll"));
#endif
    }
    void repeatedLoadsLeaveNoMediaModulesOrWorker() {
        using namespace vusiif::ztgui::package;
        QTemporaryDir settings; QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
        const auto media=inspect(MEDIA_PLUGIN); QVERIFY2(media.ok(),qPrintable(media.error));
        QSet<QString> bases; for(const auto &v:hostRuntimeContract()["baseLibraries"].toArray()) bases.insert(v.toString().toLower());
        QMap<QString,QString> forbidden;
        for(const auto &v:media.info["files"].toArray()) {
            const auto item=v.toObject(); const auto name=QFileInfo(item["path"].toString()).fileName().toLower();
            const bool compilerRuntime=name.startsWith("msvcp") || name.startsWith("vcruntime") || name.startsWith("concrt");
            if((item["role"]=="runtime-library" && !bases.contains(name) && !compilerRuntime) || (item["role"]=="qt-plugin" && item["path"].toString().contains("/multimedia/"))) forbidden.insert(name,item["sha256"].toString());
        }
        QFile observations(QCoreApplication::applicationDirPath()+"/media-process-observations.jsonl"); QVERIFY(observations.open(QIODevice::WriteOnly));
        QSet<QString> generations;
        for(int round=0;round<22;++round) {
            PluginRegistry registry; PluginLoader loader; QQmlEngine engine; QQuickItem parent;
            int ticks=0; QTimer heartbeat; heartbeat.setInterval(5); connect(&heartbeat,&QTimer::timeout,this,[&] { ++ticks; }); heartbeat.start();
            QVERIFY(loader.loadFile(voicePath,registry).ok()); QTRY_COMPARE_WITH_TIMEOUT(registry.plugins().size(),2,80000);
            QTRY_COMPARE_WITH_TIMEOUT(registry.preparationState("voice-chat"),PreparationState::Ready,15000);
            auto page=registry.page("voice-chat/rooms",&engine,&parent); QVERIFY2(page.result.ok(),qPrintable(page.result.message));
            auto *model=controller(page.page); QVERIFY(model);
            QTRY_VERIFY(model->property("inputs").toList().size()>=1); model->setProperty("inputVolume",125); model->setProperty("outputVolume",65); model->setProperty("muted",true);
            qint64 pid=0; QString generation;
            for(const auto &v:loader.catalog()) if(v.toMap()["id"]=="media") { pid=v.toMap()["pid"].toLongLong(); generation=v.toMap()["generation"].toString(); }
            QVERIFY(pid>0); QVERIFY(!generation.isEmpty()); QVERIFY(!generations.contains(generation)); generations.insert(generation);
            QJsonArray modules;
#ifdef Q_OS_WIN
            for(auto it=forbidden.cbegin();it!=forbidden.cend();++it) QVERIFY2(!GetModuleHandleW(reinterpret_cast<LPCWSTR>(it.key().utf16())),qPrintable(it.key()));
            const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,DWORD(pid));
            QVERIFY(snapshot!=INVALID_HANDLE_VALUE); MODULEENTRY32W module{}; module.dwSize=sizeof(module); bool multimedia=false;
            if(Module32FirstW(snapshot,&module)) do {
                const auto path=QString::fromWCharArray(module.szExePath),name=QFileInfo(path).fileName().toLower();
                if(!forbidden.contains(name)) continue;
                QFile binary(path); QVERIFY(binary.open(QIODevice::ReadOnly)); QCryptographicHash hash(QCryptographicHash::Sha256); QVERIFY(hash.addData(&binary));
                const auto sha=QString::fromLatin1(hash.result().toHex()); QCOMPARE(sha,forbidden[name]);
                modules.append(QJsonObject{{"path",path},{"sha256",sha}}); multimedia|=name.startsWith("qt6multimedia");
            } while(Module32NextW(snapshot,&module)); CloseHandle(snapshot); QVERIFY(multimedia);
#endif
            QPointer<QObject> alive=model; QSignalSpy finished(&loader,&PluginLoader::unloadFinished);
            QVERIFY(loader.unload("voice-chat",registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(registry.plugins().isEmpty(),10000); QVERIFY(alive.isNull());
            for(const auto &event:finished) QVERIFY2(event[1].toBool(),qPrintable(event[2].toString()));
            QCOMPARE(runtimeStatistics()["handles"].toInt(),0); QCOMPARE(runtimeStatistics()["directories"].toInt(),0); QVERIFY(ticks>2);
#ifdef Q_OS_WIN
            HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,DWORD(pid)); if(process) { QCOMPARE(WaitForSingleObject(process,0),DWORD(WAIT_OBJECT_0)); CloseHandle(process); }
            for(auto it=forbidden.cbegin();it!=forbidden.cend();++it) QVERIFY2(!GetModuleHandleW(reinterpret_cast<LPCWSTR>(it.key().utf16())),qPrintable(it.key()));
            const auto hostSnapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,GetCurrentProcessId());
            QVERIFY(hostSnapshot!=INVALID_HANDLE_VALUE); module.dwSize=sizeof(module);
            if(Module32FirstW(hostSnapshot,&module)) do { const auto path=QString::fromWCharArray(module.szExePath); QVERIFY2(!(path.contains("\\load-") && path.endsWith("\\plugin.dll")),qPrintable(path)); } while(Module32NextW(hostSnapshot,&module)); CloseHandle(hostSnapshot);
            DWORD handles=0; QVERIFY(GetProcessHandleCount(GetCurrentProcess(),&handles));
            int threads=0; const auto ts=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0); THREADENTRY32 thread{}; thread.dwSize=sizeof(thread);
            if(Thread32First(ts,&thread)) do { if(thread.th32OwnerProcessID==GetCurrentProcessId()) ++threads; } while(Thread32Next(ts,&thread)); CloseHandle(ts);
            observations.write(QJsonDocument(QJsonObject{{"round",round},{"pid",double(pid)},{"generation",generation},{"modules",modules},{"hostHandles",int(handles)},{"hostThreads",threads},{"workerExited",true},{"hostMediaModules",0}}).toJson(QJsonDocument::Compact)+'\n'); observations.flush();
#endif
        }
    }
    void panelOnlyConnectsOnClick() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QCoreApplication::setOrganizationName("ZTGUI-Test"); QCoreApplication::setApplicationName("VoicePlugin");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        const auto library=voicePath;
        QVERIFY(QFile::copy(library,directory.filePath(QFileInfo(library).fileName())));
        QVERIFY(QFile::copy(MEDIA_PLUGIN,directory.filePath("media.ztplugin")));
        QQmlEngine engine; QQuickWindow window; window.resize(920,780); window.show();
        auto registry=std::make_unique<PluginRegistry>(); auto loader=std::make_unique<PluginLoader>();
        QCOMPARE(loader->scan(directory.path(),*registry).size(),1);
        QTRY_COMPARE_WITH_TIMEOUT(registry->plugins().size(),2,80000);
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
        QCOMPARE(voiceConfig().value("volume").toInt(),35);
        click(window,item,"voiceConnect"); QTRY_VERIFY(!model->property("connected").toBool());
        QPointer<QObject> alive=model; QVERIFY(loader->unload("voice-chat",*registry).ok()); QTRY_VERIFY(alive.isNull());
        // Reloading after unloading must not retain a compiled page/controller from the DLL.
        QVERIFY(loader->loadFile(directory.filePath(QFileInfo(library).fileName()),*registry).ok());
        QTRY_COMPARE_WITH_TIMEOUT(registry->plugins().size(),2,80000);
        QTRY_COMPARE_WITH_TIMEOUT(registry->preparationState("voice-chat"),PreparationState::Ready,15000);
        page=registry->page("voice-chat/rooms",&engine,window.contentItem()); QVERIFY(page.result.ok());
        QCOMPARE(controller(page.page)->property("volume").toInt(),35);
        loader->requestShutdown(); QTRY_VERIFY(loader->shutdownComplete()); loader.reset(); registry.reset();
    }
    void liveRoomLifecycle() {
        const auto host=qEnvironmentVariable("ZTGUI_LIVE_VOICE_HOST");
        if (host.isEmpty()) QSKIP("Set ZTGUI_LIVE_VOICE_HOST to test the deployed server");
        QTemporaryDir directory; QVERIFY(directory.isValid()); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        QSettings().setValue("voice-chat/muted",true);
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy,"127.0.0.1",1));
        const auto library=voicePath; QVERIFY(QFile::copy(library,directory.filePath(QFileInfo(library).fileName())));
        QVERIFY(QFile::copy(MEDIA_PLUGIN,directory.filePath("media.ztplugin")));
        QQmlEngine firstEngine,secondEngine; QQuickItem firstParent,secondParent;
        auto firstRegistry=std::make_unique<PluginRegistry>(),secondRegistry=std::make_unique<PluginRegistry>();
        auto loader=std::make_unique<PluginLoader>(),secondLoader=std::make_unique<PluginLoader>();
        loader->scan(directory.path(),*firstRegistry); secondLoader->scan(directory.path(),*secondRegistry);
        QTRY_COMPARE_WITH_TIMEOUT(firstRegistry->plugins().size(),2,80000);
        QTRY_COMPARE_WITH_TIMEOUT(firstRegistry->preparationState("voice-chat"),PreparationState::Ready,15000);
        QTRY_COMPARE_WITH_TIMEOUT(secondRegistry->plugins().size(),2,80000);
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
        loader->requestShutdown(); secondLoader->requestShutdown(); QTRY_VERIFY(loader->shutdownComplete() && secondLoader->shutdownComplete()); loader.reset(); secondLoader.reset(); firstRegistry.reset(); secondRegistry.reset(); QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    }
    // Runs last: loading a real Qt Quick Controls page keeps Qt6Svg (shipped by
    // the media worker package) mapped in the host, which would confuse the
    // module-isolation check above if this test ran first.
    void previewSdkConsumerRendersVideo() {
        const auto probe=qEnvironmentVariable("ZTGUI_PREVIEW_PLUGIN");if(probe.isEmpty())QSKIP("Set ZTGUI_PREVIEW_PLUGIN to the independently built SDK preview package");
        QTemporaryDir directory;QVERIFY(directory.isValid());QVERIFY(QFile::copy(probe,directory.filePath("preview.ztplugin")));QVERIFY(QFile::copy(MEDIA_PLUGIN,directory.filePath("media.ztplugin")));
        QQmlEngine engine;QQuickWindow window;window.resize(1000,640);window.show();QTest::qWait(100);window.hide();window.show();QTRY_VERIFY(window.isExposed());
        PluginRegistry registry;PluginLoader loader;loader.scan(directory.path(),registry);QTRY_COMPARE_WITH_TIMEOUT(registry.plugins().size(),2,80000);QTRY_COMPARE_WITH_TIMEOUT(registry.preparationState("media-preview-probe"),PreparationState::Ready,15000);
        auto page=registry.page("media-preview-probe/preview",&engine,window.contentItem());QVERIFY2(page.result.ok(),qPrintable(page.result.message));page.page->setSize(QSizeF(1000,640));auto *model=controller(page.page);QVERIFY(model);QVERIFY(QMetaObject::invokeMethod(model,"start",Q_ARG(QString,"synthetic")));
        QTRY_VERIFY_WITH_TIMEOUT(model->property("status").toString().contains(QStringLiteral("正在回送")),10000);QTest::qWait(500);
        for(const char *name:{"sourcePreviewHost","decodedPreviewHost"}) {
            auto *host=page.page->findChild<QQuickItem *>(name);QVERIFY(host);auto grab=host->grabToImage();QVERIFY(grab);QSignalSpy ready(grab.data(),&QQuickItemGrabResult::ready);QTRY_VERIFY(!ready.isEmpty());auto image=grab->image();QVERIFY(!image.isNull());auto pixel=image.pixelColor(image.width()/5,image.height()/2);QVERIFY2(pixel.red()>180&&pixel.green()<100&&pixel.blue()<100,name);
        }
        loader.requestShutdown();QTRY_VERIFY_WITH_TIMEOUT(loader.shutdownComplete(),10000);
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
