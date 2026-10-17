#include "ZeroTierQueries.h"
#include <ztprotocol/WireProtocol.h>
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonArray>
#include <QNetworkProxy>
using namespace vusiif::ztgui;
class QueryTests : public QObject {
    Q_OBJECT
private slots:
    void wireLimitsAndFragmentation() {
        auto bytes=protocol::frame("test-echo",QByteArray("a\0b",3));
        QByteArray input=bytes.left(5); protocol::Packet packet;
        QCOMPARE(protocol::takeFrame(input,packet),protocol::ReadResult::Incomplete);
        input += bytes.mid(5)+bytes;
        QCOMPARE(protocol::takeFrame(input,packet),protocol::ReadResult::Packet);
        QCOMPARE(packet.payload,QByteArray("a\0b",3));
        QCOMPARE(protocol::takeFrame(input,packet),protocol::ReadResult::Packet); QVERIFY(input.isEmpty());
        QVERIFY(protocol::encode("../bad",{}).isEmpty());
        QVERIFY(protocol::encode("test-echo",QByteArray(protocol::MaxTcpBytes,'x')).isEmpty());
        input=QByteArray::fromHex("00100001");
        QCOMPARE(protocol::takeFrame(input,packet),protocol::ReadResult::Invalid);
        QVERIFY(!protocol::decode("ZTS1\1\0",packet));
    }
    void controllerAggregationAndCancellation() {
        QTemporaryDir directory; QFile token(directory.filePath("token"));
        QVERIFY(token.open(QIODevice::WriteOnly)); token.write("private-test-token"); token.close();
        QTcpServer http; QVERIFY(http.listen(QHostAddress::LocalHost,0));
        bool sawToken=false;
        bool delayStatus=false;
        connect(&http,&QTcpServer::newConnection,this,[&] {
            while(http.hasPendingConnections()) {
                auto *socket=http.nextPendingConnection();
                auto input=std::make_shared<QByteArray>();
                connect(socket,&QTcpSocket::readyRead,socket,[&,socket,input] {
                    *input += socket->readAll(); if(!input->contains("\r\n\r\n")) return;
                    sawToken |= input->toLower().contains("x-zt1-auth: private-test-token");
                    const auto path=input->split(' ')[1];
                    QByteArray body;
                    if(path=="/status") body="{\"online\":true}";
                    else if(path=="/peer") body="{}";
                    else if(path.endsWith("/member")) body="{\"0123456789\":1,\"abcdef0123\":2}";
                    else if(path.contains("/member/")) {
                        const auto id=path.mid(path.lastIndexOf('/')+1);
                        QJsonObject object{{"address",QString::fromLatin1(id)},{"name","mock"},{"authorized",true},{"ipAssignments",QJsonArray{"10.0.0.1"}}};
                        if (id=="abcdef0123") object.remove("name");
                        body=QJsonDocument(object).toJson(QJsonDocument::Compact);
                    } else body="[]";
                    auto respond = [socket,body] {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);
                        socket->disconnectFromHost();
                    };
                    if(path=="/status" && delayStatus) QTimer::singleShot(200,socket,respond);
                    else respond();
                });
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            }
        });
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy,"127.0.0.1",1));
        server::ZeroTierQueries queries(nullptr,QUrl(QString("http://127.0.0.1:%1").arg(http.serverPort())),token.fileName());
        bool complete=false; server::api::JsonResult result;
        queries.query("controller.members",{{"networkId","0123456789000000"}},this,[&](const auto &value){result=value;complete=true;});
        QTRY_VERIFY(complete); QVERIFY(result.ok()); QCOMPARE(result.document.array().size(),2); QVERIFY(sawToken);
        QCOMPARE(result.document.array()[0].toObject().value("ipAssignments").toArray().size(),1);
        for (const auto &member : result.document.array()) QVERIFY(member.toObject().contains("name"));
        complete=false; queries.query("peers",{},this,[&](const auto &value){result=value;complete=true;});
        QTRY_VERIFY(complete); QVERIFY(!result.ok()); QVERIFY(!result.error.contains("private-test-token"));
        complete=false; queries.query("controller.members",{{"networkId","invalid"}},this,[&](const auto &value){result=value;complete=true;});
        QTRY_VERIFY(complete); QVERIFY(!result.ok());
        delayStatus=true;
        auto *context=new QObject;
        int callbacks=0; queries.query("info",{},context,[&](const auto &){++callbacks;});
        QTest::qWait(25); delete context;
        QTest::qWait(250); QCOMPARE(callbacks,0);
        queries.query("info",{},this,[&](const auto &){++callbacks;}); QTest::qWait(25); queries.stop();
        QTest::qWait(250); QCOMPARE(callbacks,0);
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    }
};
QTEST_GUILESS_MAIN(QueryTests)
#include "QueryTests.moc"
