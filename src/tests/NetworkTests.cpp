#include "NetworkMembers.h"
#include <ztprotocol/NetworkData.h>
#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkInterface>
#include <QtTest>

using namespace vusiif::zto;
class NetworkTests : public QObject {
    Q_OBJECT
private slots:
    void serverAddress() {
        QCOMPARE(protocol::networkServer({{"assignedAddresses", QJsonArray{"fd00::2/64", "12.34.56.78/24"}}}), QHostAddress("12.34.56.1"));
        QVERIFY(protocol::networkServer({{"assignedAddresses", QJsonArray{"fd00::2/64"}}}).isNull());
    }
    void memberProtocolAndCancellation() {
        QTcpServer server;
        QVERIFY2(server.listen(QHostAddress::LocalHost, protocol::NetworkPort), qPrintable(server.errorString()));
        int received = 0;
        bool respond = true;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            auto input = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, input] {
                *input += socket->readAll();
                protocol::Packet packet;
                if (protocol::takeFrame(*input, packet) != protocol::ReadResult::Packet) return;
                QCOMPARE(packet.plugin, QByteArray("builtin"));
                const auto request = QJsonDocument::fromJson(packet.payload).object();
                QCOMPARE(request.value("op").toString(), QString("controller.members"));
                QCOMPARE(request.value("data").toObject().value("networkId").toString(), QString("0123456789abcdef"));
                ++received;
                if (!respond) return;
                const QJsonObject response{{"v",1}, {"id",request.value("id")}, {"ok",true},
                    {"data",QJsonArray{QJsonObject{{"nodeId","0123456789"},{"authorized",true}}}}};
                const auto bytes = protocol::frame("builtin", QJsonDocument(response).toJson(QJsonDocument::Compact));
                socket->write(bytes.left(5));
                QTimer::singleShot(10, socket, [socket, bytes] { socket->write(bytes.mid(5)); });
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        bool complete = false;
        util::JsonResult result;
        zerotier::requestMembers(this, QHostAddress::LocalHost, "0123456789abcdef", [&](const auto &value) { result=value; complete=true; });
        QTRY_VERIFY(complete);
        QVERIFY2(result.ok(), qPrintable(result.error));
        QCOMPARE(result.document.array().size(), 1);
        respond = false;
        auto *context = new QObject;
        bool cancelledCallback = false;
        zerotier::requestMembers(context, QHostAddress::LocalHost, "0123456789abcdef", [&](const auto &) { cancelledCallback=true; });
        QTRY_COMPARE(received, 2);
        delete context;
        QTest::qWait(40);
        QVERIFY(!cancelledCallback);
        complete = false;
        zerotier::requestMembers(this, {}, "bad", [&](const auto &value) { result=value; complete=true; });
        QVERIFY(!complete);
        QTRY_VERIFY(complete);
        QVERIFY(!result.ok());
    }
    void udpMemberSelectionAndBinaryData() {
        QHostAddress local;
        for (const auto &address : QNetworkInterface::allAddresses())
            if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback()) { local = address; break; }
        QVERIFY(!local.isNull());
        QUdpSocket receiver;
        QVERIFY2(receiver.bind(local, protocol::NetworkPort), qPrintable(receiver.errorString()));
        const QJsonArray members{QJsonObject{{"nodeId","0123456789"},{"authorized",true},{"ipAssignments",QJsonArray{local.toString()}}},
            QJsonObject{{"nodeId","abcdef0123"},{"authorized",false},{"ipAssignments",QJsonArray{local.toString()}}}};
        const auto data = QByteArray("a\0b",3);
        auto result = util::sendNetworkData(members, "0123456789", "test-data", data);
        QVERIFY2(result.ok(), qPrintable(result.error)); QCOMPARE(result.recipients,1);
        QTRY_VERIFY(receiver.hasPendingDatagrams());
        QByteArray packet(receiver.pendingDatagramSize(), '\0'); receiver.readDatagram(packet.data(), packet.size());
        protocol::Packet decoded; QVERIFY(protocol::decode(packet,decoded));
        QCOMPARE(decoded.plugin,QByteArray("test-data")); QCOMPARE(decoded.payload,data);
        QVERIFY(!util::sendNetworkData(members,"abcdef0123","test-data",data).ok());
        QVERIFY(!util::sendNetworkData(members,"invalid","test-data",data).ok());
        QVERIFY(!util::sendNetworkData(members,{},"test-data",QByteArray(protocol::MaxUdpBytes,'x')).ok());
        result = util::sendNetworkData(members,{},"test-data",data);
        QVERIFY(result.ok()); QCOMPARE(result.recipients,1);
    }
};
QTEST_GUILESS_MAIN(NetworkTests)
#include "NetworkTests.moc"
