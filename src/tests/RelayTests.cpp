#include "NetworkTransport.h"
#include <ztprotocol/NetworkDelivery.h>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QJsonDocument>
#include <QTimer>
#include <QtTest>
using namespace vusiif::ztgui;
static const QString NetworkId="0123456789abcdef",Alice="0000000002",Bob="0000000003",Carol="0000000004";
static QJsonArray descriptors(const QString &ip) { return {QJsonObject{{"nwid",NetworkId},{"assignedAddresses",QJsonArray{ip+"/24"}}}}; }
static QJsonArray memberRows() {
    QJsonArray rows;
    for(const auto &pair:QList<QPair<QString,QString>>{{"0123456789","127.0.0.1"},{Alice,"127.0.0.2"},{Bob,"127.0.0.3"},{Carol,"127.0.0.4"},{"0000000005","127.0.0.5"}})
        rows.append(QJsonObject{{"nodeId",pair.first},{"authorized",true},{"ipAssignments",QJsonArray{pair.second}}});
    return rows;
}
class Gateway : public QObject {
public:
    network::NetworkTransport transport{true}; QTcpServer tcp; QUdpSocket udp;
    int forwards=0,probeDelay=0; qint64 queuedBytes=0; bool dropAck=false,probeEnabled=true; QStringList targets;
    bool start(const QString &node,const QString &ip) {
        if(!tcp.listen(QHostAddress(ip),protocol::NetworkPort)||!udp.bind(QHostAddress(ip),protocol::NetworkPort)) return false;
        transport.setNetworks(node,descriptors(ip));
        transport.setRelayConfig({{NetworkId,QJsonArray{QJsonObject{{"nodeId","0000000005"},{"moonId","0000000000000005"},{"virtualIp","127.0.0.5"},{"port",18100}}}}});
        connect(&udp,&QUdpSocket::readyRead,this,[this] {
            while(udp.hasPendingDatagrams()) {
                const auto d=udp.receiveDatagram(1233); protocol::Packet packet; if(!protocol::decode(d.data(),packet)) continue;
                auto request=QJsonDocument::fromJson(packet.payload).object();
                if(request["op"]=="network.probe"&&!request["reply"].toBool()) {
                    if(!probeEnabled) continue; request["reply"]=true; request["bytesPerSecond"]=0; request["queuedBytes"]=double(queuedBytes);
                    QTimer::singleShot(probeDelay,this,[this,request,d] { udp.writeDatagram(protocol::encode("builtin",QJsonDocument(request).toJson(QJsonDocument::Compact)),d.senderAddress(),d.senderPort()); });
                } else transport.datagram(d.senderAddress(),d.senderPort(),packet.payload);
            }
        });
        connect(&tcp,&QTcpServer::newConnection,this,[this] {
            while(tcp.hasPendingConnections()) {
                auto *socket=tcp.nextPendingConnection(); auto input=std::make_shared<QByteArray>();
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                connect(socket,&QTcpSocket::readyRead,socket,[this,socket,input] {
                    *input+=socket->readAll(); protocol::Packet packet;
                    if(protocol::takeFrame(*input,packet)!=protocol::ReadResult::Packet) return;
                    const auto request=QJsonDocument::fromJson(packet.payload).object();
                    auto reply=[socket,request,this](QJsonObject response) {
                        if(dropAck&&request["op"]=="network.forward") return;
                        response["v"]=1; response["id"]=request["id"]; socket->write(protocol::frame("builtin",QJsonDocument(response).toJson(QJsonDocument::Compact)));
                    };
                    if(request["op"]=="controller.members") reply({{"ok",true},{"data",memberRows()}});
                    else {
                        if(request["op"]=="network.forward") { ++forwards; for(const auto &id:request["data"].toObject()["members"].toArray()) targets.append(id.toString()); }
                        transport.builtin(socket->peerAddress(),request,socket,reply);
                    }
                });
            }
        }); return true;
    }
};
class RelayTests : public QObject {
    Q_OBJECT
private slots:
    void deliveryBoundsAndIdentity() {
        protocol::Delivery d{NetworkId,Alice,Bob,"Example-plugin",QByteArray(16,'i'),QByteArray(protocol::maxNetworkDataBytes("Example-plugin"),'x')};
        const auto bytes=protocol::encodeDelivery(d); QCOMPARE(bytes.size(),protocol::MaxUdpBytes);
        protocol::Packet packet; QVERIFY(protocol::decode(bytes,packet)); protocol::Delivery parsed; QVERIFY(protocol::decodeDelivery(packet.payload,parsed)); QCOMPARE(parsed.data,d.data);
        d.data+='x'; QVERIFY(protocol::encodeDelivery(d).isEmpty()); d.data="data"; d.plugin="with.dot"; QVERIFY(protocol::encodeDelivery(d).isEmpty());
    }
    void forwardingBalancingFallbackCancellationAndAuthorization() {
        Gateway primary,extra; QVERIFY(primary.start("0123456789","127.0.0.1")); QVERIFY(extra.start("0000000005","127.0.0.5"));
        network::NetworkTransport alice,bob,carol;
        alice.setNetworks(Alice,descriptors("127.0.0.2")); bob.setNetworks(Bob,descriptors("127.0.0.3")); carol.setNetworks(Carol,descriptors("127.0.0.4"));
        int receivedBob=0,receivedCarol=0,wrongPlugin=0; QByteArray binary("a\0b",3);
        auto *sub=bob.subscribe("Example-plugin",NetworkId,this,[&](const util::NetworkMessage &m) { QCOMPARE(m.networkId,NetworkId); QCOMPARE(m.senderId,Alice); QCOMPARE(m.data,binary); ++receivedBob; }); QVERIFY(sub);
        carol.subscribe("Example-plugin",NetworkId,this,[&](const util::NetworkMessage &) { ++receivedCarol; });
        bob.subscribe("Other-plugin",NetworkId,this,[&](const util::NetworkMessage &) { ++wrongPlugin; });
        QTest::qWait(4400);
        bool queried=false; util::RelayResult relays; alice.relays(NetworkId,this,[&](const util::RelayResult &r) { relays=r; queried=true; }); QTRY_VERIFY(queried);
        QVERIFY2(relays.ok(),qPrintable(relays.error)); QCOMPARE(relays.relays.size(),2);
        for(const auto &r:relays.relays) { QVERIFY(r.available); QVERIFY(r.averageLatency>=0); QVERIFY(r.lastLatency>=0); }
        bool done=false; util::SendResult sent;
        alice.send("Example-plugin",NetworkId,{Bob,Bob.toUpper(),Carol},binary,false,this,[&](const util::SendResult &r) { sent=r;done=true; });
        QTRY_VERIFY(done); QVERIFY2(sent.ok(),qPrintable(sent.error)); QCOMPARE(sent.recipients,2); QTRY_COMPARE(receivedBob,1); QTRY_COMPARE(receivedCarol,1);
        QCOMPARE(primary.forwards,1); QCOMPARE(extra.forwards,0); QCOMPARE(wrongPlugin,0);
        done=false; alice.send("Example-plugin",NetworkId,{Bob,Carol},binary,true,this,[&](const auto &r) { sent=r;done=true; });
        QTRY_VERIFY(done); QTRY_COMPARE(receivedBob,2); QTRY_COMPARE(receivedCarol,2); QVERIFY(primary.targets.contains(Bob)||primary.targets.contains(Carol)); QVERIFY(extra.forwards>0);
        const auto primaryBefore=primary.forwards,extraBefore=extra.forwards;
        done=false; alice.send("Example-plugin",NetworkId,{Bob,Carol},binary,true,this,[&](const auto &r) { sent=r;done=true; }); QTRY_VERIFY(done);
        QCOMPARE(primary.forwards-primaryBefore,1); QCOMPARE(extra.forwards-extraBefore,1); QTRY_COMPARE(receivedBob,3); QTRY_COMPARE(receivedCarol,3);
        // Lost acknowledgement triggers direct fallback with the same message ID; the receiver deduplicates it.
        primary.dropAck=true; done=false; alice.send("Example-plugin",NetworkId,{Bob},binary,false,this,[&](const auto &r) { sent=r;done=true; }); QTRY_VERIFY(done); QTRY_COMPARE(receivedBob,4); QTest::qWait(100); QCOMPARE(receivedBob,4); primary.dropAck=false;
        bool rejected=false; primary.transport.builtin(QHostAddress("127.0.0.2"),{{"op","network.forward"},{"data",QJsonObject{{"networkId",NetworkId},{"senderId",Bob}}}},this,[&](const QJsonObject &r) { QVERIFY(!r["ok"].toBool()); rejected=true; }); QTRY_VERIFY(rejected);
        done=false; alice.send("Example-plugin",NetworkId,{},binary,false,this,[&](const auto &r) { sent=r;done=true; }); QTRY_VERIFY(done); QCOMPARE(sent.recipients,0); QVERIFY(sent.ok());
        done=false; alice.send("Example-plugin",NetworkId,{"not-a-member"},binary,false,this,[&](const auto &r) { sent=r;done=true; }); QTRY_VERIFY(done); QVERIFY(!sent.ok());
        done=false; alice.send("Example-plugin",NetworkId,{Bob,"0000000099"},binary,false,this,[&](const auto &r) { sent=r;done=true; }); QTRY_VERIFY(done); QVERIFY(!sent.ok()); QCOMPARE(sent.recipients,1); QVERIFY(sent.failedMembers.contains("0000000099")); QTRY_COMPARE(receivedBob,5);
        auto *context=new QObject; bool cancelled=false; alice.send("Example-plugin",NetworkId,{Bob},binary,false,context,[&](const auto &) { cancelled=true; }); delete context; QTest::qWait(100); QVERIFY(!cancelled);
        // Lower latency wins even when that relay reports more queued bytes.
        primary.queuedBytes=1000000; extra.probeDelay=150; QTest::qWait(6500);
        const int beforeExtra=extra.forwards; done=false; alice.send("Example-plugin",NetworkId,{Bob,Carol},binary,true,this,[&](const auto &r) { sent=r;done=true; }); QTRY_VERIFY(done); QCOMPARE(extra.forwards,beforeExtra);
        // Default relay failure must go direct rather than through the other relay.
        primary.tcp.close(); primary.udp.close(); QTest::qWait(6200); const int extraLast=extra.forwards;
        done=false; alice.send("Example-plugin",NetworkId,{Bob},binary,false,this,[&](const auto &r) { sent=r;done=true; }); QTRY_VERIFY_WITH_TIMEOUT(done,5000); QVERIFY(sent.ok()); QCOMPARE(extra.forwards,extraLast);
        sub->cancel(); const auto count=receivedBob; done=false; alice.send("Example-plugin",NetworkId,{Bob},binary,false,this,[&](const auto &) { done=true; }); QTRY_VERIFY(done); QTest::qWait(100); QCOMPARE(receivedBob,count);
    }
};
QTEST_GUILESS_MAIN(RelayTests)
#include "RelayTests.moc"
