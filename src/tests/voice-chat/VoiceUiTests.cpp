#include "VoiceController.h"
#include "AudioDeviceCache.h"
#include "VoiceSession.h"
#include <Protocol.h>
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
#include <algorithm>
using namespace vusiif::ztgui;

class RecordingLease final : public util::ServiceLease {
public:
    struct Call { QString method; QJsonObject data; QByteArray payload; };
    QList<Call> calls;
    util::ServiceCallback event;
    protocol::service::BulkChannel channel;
    QMap<QString,QJsonObject> tracks;
    int serial=0;
    quint64 generation() const override { return 1; }
    bool available() const override { return true; }
    void call(const QString &method,const QJsonObject &data,const QByteArray &payload,QObject *context,util::ServiceCallback completed) override {
        calls.append({method,data,payload});
        QJsonObject result;
        if(method=="$channel") {
            result=channel.listen(QString(32,'a'),"1");
            channel.received=[this](const QJsonObject &header,const QByteArray &bytes){calls.append({"packet",header,bytes});};
        } else if(method=="track.create") {
            auto id=QString::number(++serial);tracks[id]=data["config"].toObject();
            result={{"id",id},{"configuration","1"},{"config",tracks[id]}};
        } else if(method=="track.remove") tracks.remove(data["id"].toString());
        if(completed) QTimer::singleShot(0,context,[completed,result] { completed({result,{},{}}); });
    }
    void subscribe(QObject *,util::ServiceCallback callback) override { event=std::move(callback); }
    void close(QObject *context,util::ServiceCallback completed) override { event={}; if(context && completed) QTimer::singleShot(0,context,[completed] { completed({}); }); }
};
class IdentityService final : public util::ZeroTierService {
public:
    int requests=0;
    QString id="a1b2c3d4e5";
    void getZTInfo(QObject *context,util::JsonCallback completed) override {
        ++requests; const auto address=id;
        QTimer::singleShot(10,context,[completed,address] { completed({QJsonDocument(QJsonObject{{"address",address}}),{}}); });
    }
    void getZTPeers(QObject *,util::JsonCallback) override {}
    void getZTNetworks(QObject *,util::JsonCallback) override {}
    void getZTMoons(QObject *,util::JsonCallback) override {}
    void sendDataTo(const QString &,const QString &,const QByteArray &,QObject *,util::SendCallback) override {}
    void broadcastDataTo(const QString &,const QStringList &,const QByteArray &,QObject *,util::SendCallback) override {}
    void broadcastDataBalancedTo(const QString &,const QStringList &,const QByteArray &,QObject *,util::SendCallback) override {}
    void getNetworkRelays(const QString &,QObject *,util::RelayCallback) override {}
    util::NetworkSubscription *subscribeNetworkData(const QString &,QObject *,util::NetworkCallback) override { return nullptr; }
};
class RemarkFiles final : public util::PluginFiles {
public:
    QByteArray data;
    bool fail=false;
    util::FileResult readFile(const QString &path) override { return path=="member-remarks.json"?util::FileResult{data,{}}:util::FileResult{{},"missing"}; }
    util::FileResult writeFile(const QString &path,const QByteArray &bytes) override {
        if(fail || path!="member-remarks.json") return {{},"write failed"}; data=bytes; return {};
    }
};

class VoiceUiTests : public QObject {
    Q_OBJECT
private slots:
    void memberRemarksPersistByIdentityAndPreserveFailedWrites() {
        QTemporaryDir directory; QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        RemarkFiles files; auto cache=std::make_shared<voice_chat::AudioDeviceCache>(); cache->files=&files;
        const auto room=[](int id,const QString &nickname) { return QJsonObject{{"id",1},{"name","Room"},{"members",QJsonArray{
            QJsonObject{{"id",id},{"ztid","a1b2c3d4e5"},{"nickname",nickname},{"self",false}},
            QJsonObject{{"id",1},{"ztid","1234567890"},{"nickname","Self"},{"self",true}}}}}; };
        {
            voice_chat::VoiceController controller(nullptr,cache); auto *session=controller.findChild<voice_chat::VoiceSession *>(); QVERIFY(session);
            session->roomChanged(room(42,"Server name")); QCOMPARE(controller.members().first().toMap()["displayName"].toString(),QString("Server name"));
            controller.setMemberRemark(42,QStringLiteral(" 本地朋友 ")); QCOMPARE(controller.members().first().toMap()["displayName"].toString(),QStringLiteral("本地朋友"));
            QVERIFY(QJsonDocument::fromJson(files.data).object().contains("a1b2c3d4e5"));
            const auto saved=files.data; controller.setMemberRemark(1,"Cannot rename self"); QCOMPARE(files.data,saved);
            files.fail=true; controller.setMemberRemark(42,"Lost update"); QCOMPARE(files.data,saved);
            QCOMPARE(controller.members().first().toMap()["displayName"].toString(),QStringLiteral("本地朋友")); files.fail=false;
        }
        {
            voice_chat::VoiceController controller(nullptr,cache); auto *session=controller.findChild<voice_chat::VoiceSession *>();
            session->roomChanged(room(99,"New nickname")); QCOMPARE(controller.members().first().toMap()["displayName"].toString(),QStringLiteral("本地朋友"));
            controller.setMemberRemark(99,""); QCOMPARE(controller.members().first().toMap()["displayName"].toString(),QString("New nickname"));
            session->roomChanged(room(100,"")); QCOMPARE(controller.members().first().toMap()["displayName"].toString(),QString("a1b2c3d4e5"));
        }
        files.data="corrupted";
        {
            voice_chat::VoiceController controller(nullptr,cache); controller.findChild<voice_chat::VoiceSession *>()->roomChanged(room(42,"Server"));
            controller.setMemberRemark(42,"Cannot overwrite"); QCOMPARE(files.data,QByteArray("corrupted"));
        }
    }
    void emptyNicknameUsesIdentityAndCanCancel() {
        QTemporaryDir directory; QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        IdentityService identity; auto cache=std::make_shared<voice_chat::AudioDeviceCache>(); cache->zeroTier=&identity;
        voice_chat::VoiceController c(nullptr,cache); c.setNickname("  ");
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost,0)); c.setAddress("127.0.0.1:"+QString::number(server.serverPort()));
        c.toggleConnection(); QVERIFY(c.busy()); c.toggleConnection(); QTest::qWait(40); QVERIFY(!server.hasPendingConnections()); QVERIFY(!c.busy());
        c.toggleConnection(); QTRY_VERIFY(server.hasPendingConnections()); std::unique_ptr<QTcpSocket> socket(server.nextPendingConnection());
        QByteArray buffer; protocol::Packet packet;
        QTRY_VERIFY(socket->bytesAvailable()>=4);
        const auto size=qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(socket->peek(4).constData()));
        QTRY_VERIFY(socket->bytesAvailable()>=4+size); buffer=socket->readAll();
        QCOMPARE(protocol::takeFrame(buffer,packet),protocol::ReadResult::Packet);
        QCOMPARE(QJsonDocument::fromJson(packet.payload).object()["data"].toObject()["nickname"].toString(),identity.id);
        QCOMPARE(c.nickname(),identity.id); QCOMPARE(identity.requests,2);
        QCOMPARE(QJsonDocument::fromJson(packet.payload).object()["data"].toObject()["ztid"].toString(),identity.id);
        c.toggleConnection(); c.setNickname(""); identity.id.clear(); c.toggleConnection(); QTRY_VERIFY(!c.busy()); QVERIFY(c.status().contains(QStringLiteral("请填写昵称")));
        QVERIFY(!server.hasPendingConnections());
    }
    void microphoneLoopbackRestoresMuteAndStops() {
        QTemporaryDir directory; QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        auto lease=std::make_shared<RecordingLease>(); auto cache=std::make_shared<voice_chat::AudioDeviceCache>(); cache->audio=std::make_shared<media::MediaSession>(lease);
        voice_chat::VoiceController c(nullptr,cache); QTest::qWait(30); c.setMuted(true); c.setDeafened(true);
        lease->calls.clear(); c.setMicrophoneTesting(true); QVERIFY(c.microphoneTesting());
        QTRY_VERIFY(std::any_of(lease->calls.cbegin(),lease->calls.cend(),[](const auto &v) { return v.method=="track.start"; }));
        QTRY_COMPARE(lease->tracks.size(),2);
        QString sender;for(auto it=lease->tracks.cbegin();it!=lease->tracks.cend();++it)if(it.value()["direction"]=="send")sender=it.key();
        QVERIFY(!sender.isEmpty());QTest::qWait(30);
        media::EncodedPacket p;p.track=sender;p.codec="opus";p.generation=1;p.data="test-opus";
        auto header=media::toJson(p);header["type"]="packet";QVERIFY(lease->channel.send(header,p.data));
        QTRY_VERIFY(std::any_of(lease->calls.cbegin(),lease->calls.cend(),[](const auto &v){return v.method=="packet" && v.payload=="test-opus";}));
        c.setMicrophoneTesting(false); QVERIFY(!c.microphoneTesting()); QVERIFY(c.muted()); QVERIFY(c.deafened());
        QTRY_VERIFY(lease->tracks.isEmpty());
        const auto size=lease->calls.size();QVERIFY(lease->channel.send(header,"late-frame"));QTest::qWait(30);QCOMPARE(lease->calls.size(),size);
        c.setMicrophoneTesting(true); c.stopForUnload(); QVERIFY(!c.microphoneTesting());
    }
    void packetValidation() {
        using namespace voice_chat;
        Packet input{Audio,QByteArray(16,'x'),42,0xffffffff,"encoded"},output;
        QVERIFY(decodePacket(encodePacket(input),output));
        QCOMPARE(output.sender,input.sender); QCOMPARE(output.sequence,input.sequence); QCOMPARE(output.payload,input.payload);
        QVERIFY(!decodePacket(QByteArray(1201,'x'),output));
        auto bytes=encodePacket(input); bytes[4]=2; QVERIFY(!decodePacket(bytes,output));
        QVERIFY(encodePacket({Audio,"short",1,1,"x"}).isEmpty());
    }
    void deviceChangesDoNotRestartCapture() {
        QTemporaryDir directory;QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        auto lease=std::make_shared<RecordingLease>();auto cache=std::make_shared<voice_chat::AudioDeviceCache>();cache->audio=std::make_shared<media::MediaSession>(lease);
        voice_chat::VoiceController controller(nullptr,cache);QTest::qWait(30);controller.setMicrophoneTesting(true);QTRY_VERIFY(!lease->tracks.isEmpty());QTest::qWait(50);lease->calls.clear();
        lease->event({{{"event","sources"}},{},{}});QTRY_VERIFY(std::any_of(lease->calls.cbegin(),lease->calls.cend(),[](const auto &call){return call.method=="sources";}));QTest::qWait(50);
        QVERIFY(std::none_of(lease->calls.cbegin(),lease->calls.cend(),[](const auto &call){return call.method=="track.create"||call.method=="track.start"||call.method=="track.remove";}));controller.stopForUnload();
    }
    void devicePreferencesSurviveAsynchronousDiscovery() {
        QTemporaryDir directory; QCoreApplication::setOrganizationName("ZTGUI-Voice-UI"); QCoreApplication::setApplicationName("VoiceUI");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        const QByteArray device("saved-device-id"); QSettings().setValue("voice-chat/input",device); QSettings().setValue("voice-chat/output",device);
        voice_chat::VoiceController c(nullptr,std::make_shared<voice_chat::AudioDeviceCache>());
        QTest::qWait(20); QCOMPARE(c.input(),QString::fromLatin1(device.toHex())); QCOMPARE(c.output(),c.input());
        QCOMPARE(QSettings().value("voice-chat/input").toByteArray(),device);
        c.stopForUnload(); c.setAddress("127.0.0.1:18100"); c.setNickname("Stopped"); c.toggleConnection(); QVERIFY(!c.busy());
    }
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
        RemarkFiles files;
        auto lease=std::make_shared<RecordingLease>(); auto cache=std::make_shared<voice_chat::AudioDeviceCache>(); cache->audio=std::make_shared<media::MediaSession>(lease); cache->files=&files;
        voice_chat::VoiceController controller(nullptr,cache);
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
        auto *testButton=page->findChild<QQuickItem *>("voiceTestMicrophone"); QVERIFY(testButton);
        const auto clickTest=[&] { QTest::mouseClick(window.get(),Qt::LeftButton,{},testButton->mapToScene(QPointF(testButton->width()/2,testButton->height()/2)).toPoint()); };
        clickTest(); QTRY_VERIFY(controller.microphoneTesting()); clickTest(); QTRY_VERIFY(!controller.microphoneTesting()); clickTest(); QTRY_VERIFY(controller.microphoneTesting());
        QVERIFY(QDir().mkpath("screenshots"));
        QVERIFY(window->grabWindow().save(dark?"screenshots/voice-audio-dark.png":"screenshots/voice-audio-light.png"));
        QTest::keyClick(window.get(),Qt::Key_Escape); QTRY_VERIFY(!popup->property("visible").toBool());
        QVERIFY(!controller.microphoneTesting());
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
        auto *session=controller.findChild<voice_chat::VoiceSession *>(); QVERIFY(session);
        session->roomChanged({{"id",1},{"name","Room A"},{"members",QJsonArray{QJsonObject{{"id",42},{"ztid","a1b2c3d4e5"},{"nickname","Remote"},{"self",false},{"volume",100}},QJsonObject{{"id",1},{"nickname","Self"},{"self",true}}}}});
        auto *members=page->findChild<QQuickItem *>("voiceMembers"); QVERIFY(members); QTest::qWait(220);
        QVERIFY(!page->findChild<QObject *>("voiceMemberVolume"));
        for(const auto button:{Qt::LeftButton,Qt::RightButton}) {
            QTest::mouseClick(window.get(),button,{},members->mapToScene(QPointF(30,18)).toPoint());
            auto *memberPopup=page->findChild<QObject *>("voiceMemberPopup"); QTRY_VERIFY(memberPopup && memberPopup->property("visible").toBool());
            auto *slider=page->findChild<QQuickItem *>("voiceMemberVolume"); QVERIFY(slider); QTest::qWait(200);
            QTest::mouseClick(window.get(),Qt::LeftButton,{},slider->mapToScene(QPointF(slider->width()*.7,slider->height()/2)).toPoint());
            QVERIFY(controller.memberVolume(42)>100);
            auto *remark=page->findChild<QQuickItem *>("voiceMemberRemark"); QVERIFY(remark); QVERIFY(remark->isEnabled());
            remark->setProperty("text",QStringLiteral("本地朋友"));
            QVERIFY(window->grabWindow().save(dark?"screenshots/voice-member-remark-dark.png":"screenshots/voice-member-remark-light.png"));
            auto *save=page->findChild<QQuickItem *>("voiceSaveMemberRemark"); QVERIFY(save);
            QTest::mouseClick(window.get(),Qt::LeftButton,{},save->mapToScene(QPointF(save->width()/2,save->height()/2)).toPoint());
            QTRY_COMPARE(controller.members().first().toMap()["displayName"].toString(),QStringLiteral("本地朋友"));
            QTRY_VERIFY(!memberPopup->property("visible").toBool());
            QTest::keyClick(window.get(),Qt::Key_Escape); QTRY_VERIFY(!memberPopup->property("visible").toBool()); QTest::qWait(160);
        }
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
