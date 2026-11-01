#include <desktop-share/ControlProtocol.h>
#include <desktop-share/StreamReceiver.h>
#include <ztprotocol/WireProtocol.h>
#include "NetworkClient.h"
#include "DeskShareController.h"
#include <QTcpSocket>
#include <QTcpServer>
#include <QUdpSocket>
#include <QNetworkProxy>
#include <QAbstractEventDispatcher>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <atomic>
#include <QtTest>
#include <cstring>

using namespace vusiif::zto::desktop_share;
namespace wire = vusiif::zto::protocol;
namespace util = vusiif::zto::util;
namespace media = vusiif::zto::media;

// Uses the public lease contract to hold old asynchronous results until after
// a newer control epoch has arrived. No host or media implementation is linked.
class DelayedMediaLease final : public util::ServiceLease {
public:
    struct Create { QJsonObject config; QPointer<QObject> context; util::ServiceCallback done; };
    vusiif::zto::protocol::service::BulkChannel channel;
    QList<Create> creates;
    QStringList removed, started;
    quint64 generation() const override { return 1; }
    bool available() const override { return true; }
    void subscribe(QObject *,util::ServiceCallback) override {}
    void close(QObject *,util::ServiceCallback done) override { channel.close(); if(done)done({}); }
    void call(const QString &method,const QJsonObject &data,const QByteArray &,QObject *context,
              util::ServiceCallback done) override {
        if(method==QStringLiteral("$channel")) {
            done({channel.listen(QString(32,'a'),QStringLiteral("1")),{}, {}});
        } else if(method==QStringLiteral("track.create")) {
            creates.append({data["config"].toObject(),context,std::move(done)});
        } else {
            if(method==QStringLiteral("track.remove")) removed.append(data["id"].toString());
            if(method==QStringLiteral("track.start")) started.append(data["id"].toString());
            if(done) QTimer::singleShot(0,context,[done]{done({});});
        }
    }
    void complete(int index,const QString &id) {
        auto pending=creates.at(index);
        if(pending.context && pending.done)
            pending.done({{{"id",id},{"configuration",QStringLiteral("1")},{"config",pending.config}}, {}, {}});
    }
};

static MediaHeader idForTest() {
    MediaHeader h;
    h.token.fill(0x41); h.sessionId=7; h.roomId=11; h.streamEpoch=3;
    return h;
}
static VideoPacket packet(const MediaHeader &id, quint64 seq, bool key) {
    VideoPacket p;
    p.token=id.token; p.sessionId=id.sessionId; p.roomId=id.roomId;
    p.streamEpoch=id.streamEpoch; p.sequence=seq; p.pts=qint64(seq)*33333;
    p.duration=33333; p.data=QByteArray(100,'x');
    if(key) p.flags|=quint16(MediaFlag::KeyFrame);
    return p;
}
static void push(StreamReceiver &r,const MediaHeader &id,quint64 seq,bool key,qint64 now,QList<VideoPacket> &out) {
    for(const auto &f:fragmentVideo(packet(id,seq,key))) r.push(f,now,out);
}

struct Publisher {
    QTcpSocket tcp; QUdpSocket udp; QByteArray incoming;
    quint32 next=1; quint64 session=0; RoomInfo room; quint64 epoch=0;
    Response request(Request request) {
        request.id=next++;
        tcp.write(wire::frame(QByteArray(Route),encodeRequest(request)));
        tcp.waitForBytesWritten(3000);
        QElapsedTimer timeout; timeout.start();
        while(timeout.elapsed()<3000) {
            wire::Packet frame;
            const auto status=wire::takeFrame(incoming,frame);
            if(status==wire::ReadResult::Packet) {
                Response response;
                if(decodeResponse(frame.payload,request.op,response).ok && response.id==request.id)
                    return response;
                continue;
            }
            if(status==wire::ReadResult::Invalid) break;
            if(tcp.bytesAvailable() || tcp.waitForReadyRead(100)) incoming+=tcp.readAll();
        }
        return {};
    }
    bool open(const QString &codec = QStringLiteral("h264")) {
        tcp.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        tcp.connectToHost(QHostAddress::LocalHost,18140);
        if(!tcp.waitForConnected(3000)) return false;
        Request q; q.op=Operation::Hello;
        const auto hello=request(q); if(!hello.ok) return false;
        session=hello.hello.sessionId;
        q={};q.op=Operation::Create;q.profile=QStringLiteral("720p30");
        const auto created=request(q); if(!created.ok) return false; room=created.room;
        q={};q.op=Operation::StreamConfigure;q.roomId=room.roomId;
        q.format.codec=codec;q.format.width=1280;q.format.height=720;
        q.format.fps=30;q.format.bitrate=2000000;
        const auto configured=request(q); if(!configured.ok) return false; epoch=configured.streamEpoch;
        if(!udp.bind(QHostAddress(QHostAddress::LocalHost),0)) return false;
        std::array<quint8,16> token{};std::memcpy(token.data(),room.token.constData(),16);
        const auto bytes=wrapZts1(encodeRegister(token,session,room.roomId));
        udp.writeDatagram(bytes,QHostAddress::LocalHost,18140);
        return udp.waitForReadyRead(3000);
    }
    void send(quint64 seq,bool key) {
        MediaHeader id;id.sessionId=session;id.roomId=room.roomId;id.streamEpoch=epoch;
        std::memcpy(id.token.data(),room.token.constData(),16);
        for(const auto &f:fragmentVideo(packet(id,seq,key)))
            udp.writeDatagram(wrapZts1(f),QHostAddress::LocalHost,18140);
    }
};

// Scripted TCP control host for connection/handshake regressions. It answers
// (or deliberately ignores) the control handshake only. It is not forwarding
// evidence: real forwarding stays on the real Java host at 18140.
class ScriptedHost : public QObject
{
public:
    enum class Mode { MissingPlugin, UnreadyPlugin, Silent, Valid, StaleHelloThenValid,
                      UnrelatedBuiltinThenValid };

    explicit ScriptedHost(Mode mode, QObject *parent = nullptr)
        : QObject(parent), m_mode(mode)
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = m_server.nextPendingConnection()) {
                m_buffers.insert(socket, QByteArray());
                QObject::connect(socket, &QTcpSocket::readyRead, this,
                                 [this, socket] { read(socket); });
                QObject::connect(socket, &QTcpSocket::disconnected, socket,
                                 &QObject::deleteLater);
            }
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return m_server.serverPort(); }

    QList<Operation> operations;
    int helloCount = 0;
    int unrelatedBuiltinSent = 0;
    quint32 firstHelloId = 0;

private:
    static QByteArray builtinPayload(const QByteArray &plugin, const QByteArray &error)
    {
        QJsonObject object;
        object.insert(QStringLiteral("v"), 1);
        object.insert(QStringLiteral("ok"), false);
        object.insert(QStringLiteral("error"), QString::fromLatin1(error));
        object.insert(QStringLiteral("plugin"), QString::fromLatin1(plugin));
        return QJsonDocument(object).toJson(QJsonDocument::Compact);
    }
    static void sendHello(QTcpSocket *socket, quint32 id, quint64 session)
    {
        Response response;
        response.id = id;
        response.ok = true;
        response.op = Operation::Hello;
        response.hello.sessionId = session;
        response.hello.udpPort = 18199;
        response.hello.maxViewers = 8;
        response.hello.profiles = {QStringLiteral("720p30")};
        socket->write(wire::frame(QByteArray(Route), encodeResponse(response)));
    }
    void read(QTcpSocket *socket)
    {
        auto &buffer = m_buffers[socket];
        buffer += socket->readAll();
        for (;;) {
            wire::Packet frame;
            const auto status = wire::takeFrame(buffer, frame);
            if (status == wire::ReadResult::Incomplete) return;
            if (status == wire::ReadResult::Invalid) {
                socket->abort();
                return;
            }
            if (frame.plugin != QByteArray(Route)) continue;
            Request request;
            if (!decodeRequest(frame.payload, request).ok) continue;
            operations.append(request.op);
            if (request.op != Operation::Hello) continue;
            ++helloCount;
            if (helloCount == 1) firstHelloId = request.id;
            handleHello(socket, request);
        }
    }
    void handleHello(QTcpSocket *socket, const Request &request)
    {
        switch (m_mode) {
        case Mode::MissingPlugin:
        case Mode::UnreadyPlugin:
            socket->write(wire::frame(QByteArray("builtin"),
                                     builtinPayload(QByteArray(Route),
                                                    QByteArray("unknown_or_unready_plugin"))));
            break;
        case Mode::Silent:
            break;
        case Mode::Valid:
            sendHello(socket, request.id, 4242);
            break;
        case Mode::StaleHelloThenValid:
            if (helloCount == 1) break; // first attempt: no answer -> deadline
            // Replay the first attempt's hello with its old request id on the
            // new connection before answering the current hello.
            sendHello(socket, firstHelloId, 9876);
            sendHello(socket, request.id, 4242);
            break;
        case Mode::UnrelatedBuiltinThenValid:
            // A neighbour plugin's missing error and another builtin error for
            // our route are not handshake answers and must not end the attempt.
            socket->write(wire::frame(QByteArray("builtin"),
                                     builtinPayload(QByteArray("other-plugin"),
                                                    QByteArray("unknown_or_unready_plugin"))));
            socket->write(wire::frame(QByteArray("builtin"),
                                     builtinPayload(QByteArray(Route),
                                                    QByteArray("unknown_builtin_operation"))));
            ++unrelatedBuiltinSent;
            {
                const QPointer<QTcpSocket> guard(socket);
                const quint32 id = request.id;
                QTimer::singleShot(200, this, [guard, id] {
                    if (guard) sendHello(guard, id, 4242);
                });
            }
            break;
        }
    }

    Mode m_mode;
    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

class Stage4RegressionTests:public QObject {
    Q_OBJECT
private slots:
    void realReceiverUsesCodecDependencyRules_data() {
        QTest::addColumn<QString>("codec");
        QTest::newRow("h264") << QStringLiteral("h264");
        QTest::newRow("absolute-tiles") << QStringLiteral("bgra-tiles");
    }
    void realReceiverUsesCodecDependencyRules() {
        QFETCH(QString,codec);
        const bool tiles = codec == QStringLiteral("bgra-tiles");
        Publisher pub; QVERIFY(pub.open(codec));
        NetworkClient viewer; bool connected=false, joined=false, ready=false;
        connect(&viewer,&NetworkClient::connected,this,[&]{connected=true;});
        connect(&viewer,&NetworkClient::roomJoined,this,[&]{joined=true;});
        connect(&viewer,&NetworkClient::streamReadyConfirmed,this,[&]{ready=true;});
        viewer.start(); viewer.connectToServer(QStringLiteral("127.0.0.1"),18140);
        QTRY_VERIFY_WITH_TIMEOUT(connected,3000);
        viewer.joinRoom(pub.room.roomCode); QTRY_VERIFY_WITH_TIMEOUT(joined,3000);
        viewer.beginReceive(pub.epoch); viewer.streamReady(pub.epoch);
        QTRY_VERIFY_WITH_TIMEOUT(ready,3000);
        QList<VideoPacket> out;
        pub.send(0,true); QTRY_VERIFY_WITH_TIMEOUT(viewer.stats().inboundDelivered>=1,3000);
        viewer.takeVideo(out); QCOMPARE(out.size(),1); out.clear();
        pub.send(2,true); QTest::qWait(70); viewer.takeVideo(out);
        QCOMPARE(out.size(),tiles ? 0 : 1); out.clear();
        pub.send(1,true); QTest::qWait(70); viewer.takeVideo(out);
        QCOMPARE(out.size(),tiles ? 2 : 0);
        if(tiles){QCOMPARE(out[0].sequence,quint64(1));QCOMPARE(out[1].sequence,quint64(2));}
        out.clear(); pub.send(4,false); QTest::qWait(300); viewer.takeVideo(out);
        QCOMPARE(out.size(),tiles ? 1 : 0);
        if(tiles) QCOMPARE(out.first().sequence,quint64(4));
        QVERIFY(viewer.shutdown());
    }
    void remainingGapMustKeepIts200msDeadline() {
        const auto id=idForTest(); StreamReceiver r;r.configure(id,true,0);
        QList<VideoPacket> out;
        push(r,id,0,true,0,out);out.clear();
        push(r,id,3,false,1000,out); // packets 1 and 2 absent
        push(r,id,1,false,2000,out); // packet 1 now arrives; 2 still absent
        QCOMPARE(out.size(),1);out.clear();
        const bool gap=r.tick(201001,out);
        qInfo()<<"remaining-gap after deadline: gap="<<gap<<"held="<<r.stats().heldPackets
               <<"waitingKey="<<r.waitingKeyFrame();
        QVERIFY2(gap && r.waitingKeyFrame(),"Sequence 2 is lost but its deadline was disarmed when sequence 1 arrived");
    }
    void recoveryKeyMustNotRewindTheCommittedSequence() {
        const auto id=idForTest();StreamReceiver r;r.configure(id,true,0);
        QList<VideoPacket> out;
        push(r,id,10,true,0,out);out.clear();
        push(r,id,12,false,1000,out);
        QVERIFY(r.tick(201001,out));QVERIFY(r.waitingKeyFrame());
        push(r,id,9,true,202000,out); // delayed old keyframe, never previously completed
        qInfo()<<"old-key recovery output count="<<out.size()
               <<"sequence="<<(out.isEmpty()?999:out.first().sequence);
        QVERIFY2(out.isEmpty(),"A delayed keyframe older than committed sequence 10 must not become the recovery baseline");
    }
    void guiMailboxOverflowMustNotDeliverDependentH264() {
        Publisher pub;QVERIFY2(pub.open(),"Start the isolated real Java host at 127.0.0.1:18140");
        NetworkClient viewer;
        bool connected=false,joined=false,registered=false,ready=false;
        connect(&viewer,&NetworkClient::connected,this,[&](quint64,quint16){connected=true;});
        connect(&viewer,&NetworkClient::roomJoined,this,[&](quint64,const QString&,const QString&,
                const QByteArray&,bool,const StreamFormat&){joined=true;});
        connect(&viewer,&NetworkClient::udpReady,this,[&](bool ok,const QString&){registered=ok;});
        connect(&viewer,&NetworkClient::streamReadyConfirmed,this,[&](quint64){ready=true;});
        viewer.start();viewer.connectToServer(QStringLiteral("127.0.0.1"),18140);
        QTRY_VERIFY_WITH_TIMEOUT(connected,3000);
        viewer.joinRoom(pub.room.roomCode);
        QTRY_VERIFY_WITH_TIMEOUT(joined && registered,3000);
        viewer.beginReceive(pub.epoch);viewer.streamReady(pub.epoch);
        QTRY_VERIFY_WITH_TIMEOUT(ready,3000);
        // No GUI consumer: the real network thread fills its bounded inbox.
        for(quint64 seq=0;seq<8;++seq){pub.send(seq,seq==0);QTest::qSleep(15);}
        QTRY_VERIFY_WITH_TIMEOUT(viewer.stats().inboundDroppedBudget>0,3000);
        QList<VideoPacket> out;viewer.takeVideo(out);
        QCOMPARE(out.size(),4);
        const auto dropped=viewer.stats().inboundDroppedBudget;
        out.clear();
        pub.send(8,false);
        QTRY_VERIFY_WITH_TIMEOUT(viewer.stats().receiver.completePackets>=9,3000);
        viewer.takeVideo(out);
        qInfo()<<"mailbox drops="<<dropped<<"post-drop output="<<out.size()
               <<"sequence="<<(out.isEmpty()?999:out.first().sequence)
               <<"key="<<(!out.isEmpty() && out.first().keyFrame());
        QVERIFY2(out.isEmpty(),"H.264 packets 4-7 were lost in the GUI mailbox but dependent packet 8 was still delivered");
    }
    void shutdownTimeoutMustCoverTheNetworkDrain() {
        NetworkClient client;client.start();
        auto *thread=client.findChild<QThread*>();QVERIFY(thread);
        QTRY_VERIFY_WITH_TIMEOUT(QAbstractEventDispatcher::instance(thread)!=nullptr,3000);
        auto *dispatcher=QAbstractEventDispatcher::instance(thread);
        std::atomic<bool> busy=false;
        QMetaObject::invokeMethod(dispatcher,[&]{busy=true;QThread::msleep(250);},Qt::QueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(busy.load(),3000);
        QElapsedTimer elapsed;elapsed.start();
        const bool stopped=client.shutdown(10);
        qInfo()<<"shutdown timeout=10ms actual="<<elapsed.elapsed()<<"reported stopped="<<stopped;
        QVERIFY2(elapsed.elapsed()<100,"BlockingQueuedConnection bypasses the supplied shutdown deadline");
    }
    void explicitServerPortMustSurviveThePropertySetter() {
        DeskShareController controller({});
        controller.setServerAddress(QStringLiteral("127.0.0.1:18140"));
        controller.connectServer();
        qInfo()<<"requested 127.0.0.1:18140, status="<<controller.statusText();
        QVERIFY2(controller.statusText().contains(QStringLiteral(":18140")),
            "setServerAddress stripped the explicit port and connectServer replaced it with 18100");
    }
    void missingPluginRealHostEndsAttemptAndExplicitRetryConnects() {
        // R7: the real isolated host at 18142 listens but has no desktop-share
        // plugin. The builtin unknown_or_unready_plugin answer must terminate
        // the attempt instead of waiting for a generic control timeout.
        DeskShareController controller({});
        controller.setServerAddress(QStringLiteral("127.0.0.1:18142"));
        controller.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(controller.networkState() != QStringLiteral("connecting"), 3000);
        QCOMPARE(controller.networkState(), QStringLiteral("failed"));
        QVERIFY(!controller.connected());
        qInfo() << "real missing-plugin status:" << controller.statusText();
        QVERIFY2(controller.statusText().contains(QStringLiteral("插件")),
                 qPrintable(controller.statusText()));
        QVERIFY(controller.statusText().contains(QStringLiteral("desktop-share")));
        // Explicit retry against a real host that has the plugin.
        controller.setServerAddress(QStringLiteral("127.0.0.1:18140"));
        controller.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(controller.connected(), 3000);
        QCOMPARE(controller.networkState(), QStringLiteral("connected"));
        QVERIFY(controller.shutdown());
    }
    void unreadyPluginResponseEndsAttemptAndDropsPendingCreate() {
        ScriptedHost unready(ScriptedHost::Mode::UnreadyPlugin);
        QVERIFY(unready.listen());
        ScriptedHost valid(ScriptedHost::Mode::Valid);
        QVERIFY(valid.listen());
        DeskShareController controller({});
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(unready.port()));
        // Arms a pending create: a later reconnect must not auto-create.
        controller.createRoom();
        QCOMPARE(controller.networkState(), QStringLiteral("connecting"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.networkState() == QStringLiteral("failed"), 3000);
        QVERIFY(!controller.connected());
        QVERIFY(controller.statusText().contains(QStringLiteral("插件")));
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(valid.port()));
        controller.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(controller.connected(), 3000);
        QCOMPARE(controller.networkState(), QStringLiteral("connected"));
        QTest::qWait(300);
        QCOMPARE(valid.operations.size(), 1);
        QCOMPARE(int(valid.operations.first()), int(Operation::Hello));
        QVERIFY(!controller.publishing());
        QVERIFY(controller.shutdown());
    }
    void tcpAcceptedWithoutHelloTimesOutAndAllowsExplicitRetry() {
        ScriptedHost silent(ScriptedHost::Mode::Silent);
        QVERIFY(silent.listen());
        ScriptedHost valid(ScriptedHost::Mode::Valid);
        QVERIFY(valid.listen());
        DeskShareController controller({});
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(silent.port()));
        QElapsedTimer elapsed;
        elapsed.start();
        controller.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(controller.networkState() == QStringLiteral("failed"), 7000);
        qInfo() << "no-hello deadline elapsed ms:" << elapsed.elapsed()
                << "status:" << controller.statusText();
        // TCP was accepted immediately; the failure must follow the fixed
        // five-second connection/handshake deadline, not an earlier abort.
        QVERIFY(elapsed.elapsed() >= 4800);
        QVERIFY(elapsed.elapsed() <= 6500);
        QVERIFY(!controller.connected());
        QVERIFY(controller.statusText().contains(QStringLiteral("握手"))
                || controller.statusText().contains(QStringLiteral("超时")));
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(valid.port()));
        controller.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(controller.connected(), 3000);
        QVERIFY(controller.shutdown());
    }
    void lateHelloFromOldAttemptDoesNotPolluteNewAttempt() {
        ScriptedHost host(ScriptedHost::Mode::StaleHelloThenValid);
        QVERIFY(host.listen());
        DeskShareController controller({});
        auto *network = controller.findChild<NetworkClient *>();
        QVERIFY(network);
        QSignalSpy connections(network, &NetworkClient::connected);
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(controller.networkState() == QStringLiteral("failed"), 7000);
        QCOMPARE(host.helloCount, 1);
        controller.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(controller.connected(), 3000);
        QCOMPARE(connections.size(), 1);
        QCOMPARE(connections.first().at(0).toULongLong(), quint64(4242));
        QCOMPARE(host.helloCount, 2);
        QVERIFY2(network->stats().lateResponses >= 1,
                 "The stale hello of the timed-out attempt must be counted as late");
        QVERIFY(controller.shutdown());
    }
    void unrelatedBuiltinMessagesDoNotChangeTheAttempt() {
        ScriptedHost host(ScriptedHost::Mode::UnrelatedBuiltinThenValid);
        QVERIFY(host.listen());
        DeskShareController controller({});
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        QTRY_COMPARE_WITH_TIMEOUT(host.unrelatedBuiltinSent, 1, 3000);
        // Neighbour-plugin and unrelated builtin errors were just handled; the
        // attempt must still be a live handshake, not a failure or a connection.
        QCOMPARE(controller.networkState(), QStringLiteral("connecting"));
        QVERIFY(!controller.connected());
        QTRY_VERIFY_WITH_TIMEOUT(controller.connected(), 3000);
        QCOMPARE(controller.networkState(), QStringLiteral("connected"));
        QVERIFY(controller.shutdown());
    }
    void controllerStopFailureRetainsStateAndCanRetry() {
        DeskShareController controller({});
        auto *network = controller.findChild<NetworkClient*>(); QVERIFY(network);
        auto *thread = network->findChild<QThread*>(); QVERIFY(thread);
        QTRY_VERIFY_WITH_TIMEOUT(QAbstractEventDispatcher::instance(thread),3000);
        std::atomic<bool> busy=false;
        QMetaObject::invokeMethod(QAbstractEventDispatcher::instance(thread),[&] {
            busy=true; QThread::msleep(250);
        },Qt::QueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(busy.load(),3000);
        bool done=false, ok=true, responsive=false;
        QTimer::singleShot(0,&controller,[&]{responsive=true;});
        QElapsedTimer elapsed; elapsed.start();
        controller.shutdownAsync([&](bool result){ok=result;done=true;},10);
        QVERIFY(elapsed.elapsed()<100);
        QTRY_VERIFY_WITH_TIMEOUT(done,100);
        QVERIFY(!ok); QVERIFY(responsive);
        QCOMPARE(controller.state(),QStringLiteral("stop-failed"));
        QVERIFY(network->running());
        done=false;
        controller.shutdownAsync([&](bool result){ok=result;done=true;},1000);
        QTRY_VERIFY_WITH_TIMEOUT(done,1500);
        QVERIFY(ok); QVERIFY(!network->running());
        QCOMPARE(controller.state(),QStringLiteral("stopped"));
    }
    void customPortConnectEditReconnectAndDefault() {
        DeskShareController controller({});
        auto *network=controller.findChild<NetworkClient*>();QVERIFY(network);
        QSignalSpy connections(network,&NetworkClient::connected);
        controller.setServerAddress(QStringLiteral("127.0.0.1:18140"));
        QCOMPARE(controller.serverAddress(),QStringLiteral("127.0.0.1:18140"));
        controller.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(controller.connected(),3000);
        QCOMPARE(controller.serverAddress(),QStringLiteral("127.0.0.1:18140"));
        QCOMPARE(connections.size(),1);
        QCOMPARE(connections.last().at(1).value<quint16>(),quint16(18140));
        controller.disconnectServer();
        controller.connectServer();
        QVERIFY(controller.statusText().contains(QStringLiteral(":18140")));
        QTRY_VERIFY_WITH_TIMEOUT(controller.connected(),3000);
        QCOMPARE(connections.size(),2);
        QCOMPARE(connections.last().at(1).value<quint16>(),quint16(18140));
        QCOMPARE(controller.serverAddress(),QStringLiteral("127.0.0.1:18140"));
        controller.disconnectServer();
        controller.setServerAddress(QStringLiteral("127.0.0.1"));
        controller.connectServer();
        QVERIFY(controller.statusText().contains(QStringLiteral(":18100")));
        controller.disconnectServer();
        controller.setServerAddress(QStringLiteral("127.0.0.1:18140"));
        controller.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(controller.connected(),3000);
        QVERIFY(controller.shutdown());
    }
    void queuedNetworkCallbacksAreInvalidAfterShutdown() {
        NetworkClient client; client.start();
        int delivered=0;
        connect(&client,&NetworkClient::connected,this,[&]{++delivered;});
        client.connectToServer(QStringLiteral("127.0.0.1"),18140);
        QTest::qSleep(100);
        QVERIFY(client.shutdown());
        QCoreApplication::processEvents();
        QCOMPARE(delivered,0);
        QCOMPARE(client.stats().outboundQueued,0);
        QCOMPARE(client.stats().inboundQueued,0);
    }
    void newerControlEpochInvalidatesOldMediaCreateAndStartResults() {
        auto lease=std::make_shared<DelayedMediaLease>();
        auto session=std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        auto *network=controller.findChild<NetworkClient*>(); QVERIFY(network);
        emit network->roomJoined(42,QString(),QStringLiteral("720p30"),QByteArray(16,'a'),false,{});
        StreamFormat format;format.codec=QStringLiteral("h264");format.width=1280;
        format.height=720;format.fps=30;format.bitrate=2000000;format.streamEpoch=2;
        emit network->streamConfigReceived(42,format);
        QTRY_COMPARE_WITH_TIMEOUT(lease->creates.size(),1,3000);
        format.streamEpoch=3;
        emit network->streamConfigReceived(42,format);
        QTRY_COMPARE_WITH_TIMEOUT(lease->creates.size(),2,3000);
        lease->complete(0,QStringLiteral("old-epoch"));
        QVERIFY(lease->removed.contains(QStringLiteral("old-epoch")));
        QVERIFY(!lease->started.contains(QStringLiteral("old-epoch")));
        lease->complete(1,QStringLiteral("current-epoch"));
        controller.leaveRoom(); // invalidate an already queued start completion
        QCoreApplication::processEvents();
        QVERIFY(!controller.watching());
        QCOMPARE(controller.statistics()["viewEpoch"].toString(),QStringLiteral("0"));
        QVERIFY(controller.shutdown());
    }
    void videoBeforeControlReadyAndOldEpochAfterSwitchAreRejected() {
        Publisher pub; QVERIFY(pub.open());
        NetworkClient viewer; bool joined=false, registered=false, connected=false, ready=false;
        connect(&viewer,&NetworkClient::connected,this,[&]{connected=true;});
        connect(&viewer,&NetworkClient::roomJoined,this,[&]{joined=true;});
        connect(&viewer,&NetworkClient::udpReady,this,[&](bool ok,const QString&){registered=ok;});
        connect(&viewer,&NetworkClient::streamReadyConfirmed,this,[&]{ready=true;});
        viewer.start();viewer.connectToServer(QStringLiteral("127.0.0.1"),18140);
        QTRY_VERIFY_WITH_TIMEOUT(connected,3000); viewer.joinRoom(pub.room.roomCode);
        QTRY_VERIFY_WITH_TIMEOUT(joined && registered,3000);
        viewer.beginReceive(pub.epoch);viewer.streamReady(pub.epoch);
        QTRY_VERIFY_WITH_TIMEOUT(ready,3000);
        pub.send(0,true); QTRY_COMPARE_WITH_TIMEOUT(viewer.stats().inboundDelivered,quint64(1),3000);
        QList<VideoPacket> out;viewer.takeVideo(out);out.clear();
        const auto old=pub.epoch;
        Request q;q.op=Operation::StreamConfigure;q.roomId=pub.room.roomId;
        q.format.codec=QStringLiteral("h264");q.format.width=1280;q.format.height=720;
        q.format.fps=30;q.format.bitrate=2000000;
        const auto configured=pub.request(q);QVERIFY(configured.ok);pub.epoch=configured.streamEpoch;
        ready=false;viewer.streamReady(pub.epoch);
        QTRY_VERIFY_WITH_TIMEOUT(ready,3000);
        pub.send(0,true); // new UDP while the receive identity is still old
        QTRY_VERIFY_WITH_TIMEOUT(viewer.stats().inboundRejected>0,3000);
        viewer.takeVideo(out);QVERIFY(out.isEmpty());
        viewer.beginReceive(pub.epoch);
        QTest::qWait(30);
        const auto rejected=viewer.stats().receiver.rejectedDatagrams;
        // Server rejects a stale publisher epoch; the client identity is also
        // covered directly by StreamLogicTests and the original golden suite.
        const auto current=pub.epoch;pub.epoch=old;pub.send(50,true);pub.epoch=current;
        pub.send(1,true);
        QTRY_COMPARE_WITH_TIMEOUT(viewer.stats().receiver.submittedPackets,quint64(1),3000);
        viewer.takeVideo(out);QCOMPARE(out.size(),1);
        QCOMPARE(out.first().streamEpoch,current);QCOMPARE(out.first().sequence,quint64(1));
        Q_UNUSED(rejected);
    }
};
QTEST_GUILESS_MAIN(Stage4RegressionTests)
#include "Stage4RegressionTests.moc"
