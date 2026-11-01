#include "../../client/main/plugins/ServiceBroker.h"
#include "PluginPackage.h"
#include <desktop-share/PacketReassembler.h>
#include <ztmedia/MediaSession.h>
#include <QImage>
#include <QNetworkDatagram>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <QtTest>

using namespace vusiif::zto;
namespace ds=vusiif::zto::desktop_share;
namespace {
template<class T,class F> media::Result<T> await(F operation,int timeout=10000) {
    QObject scope; media::Result<T> result; bool complete=false;
    operation(&scope,[&](media::Result<T> value){result=std::move(value);complete=true;});
    QElapsedTimer timer; timer.start();
    while(!complete && timer.elapsed()<timeout) QTest::qWait(5);
    if(!complete) result.error={"timeout","Operation timed out"};
    return result;
}
ds::MediaHeader identity() {
    ds::MediaHeader header; header.token.fill(0x31); header.sessionId=2; header.roomId=3; header.streamEpoch=4;
    return header;
}
ds::VideoPacket video(const ds::MediaHeader &id,quint64 sequence,QByteArray data) {
    ds::VideoPacket packet; packet.token=id.token; packet.sessionId=id.sessionId;
    packet.roomId=id.roomId; packet.streamEpoch=id.streamEpoch; packet.sequence=sequence;
    packet.duration=33333; packet.data=std::move(data); return packet;
}
}
class TileUdpTests : public QObject {
    Q_OBJECT
private slots:
    void boundedReassemblyAndConflict() {
        const auto id=identity(); ds::PacketReassembler assembler(id); ds::VideoPacket output;
        QList<QByteArray> first;
        for(int i=0;i<9;++i) {
            const auto fragments=ds::fragmentVideo(video(id,i,QByteArray(ds::MaxVideoPacketBytes,'x')));
            QVERIFY(!fragments.isEmpty()); if(!i)first=fragments;
            QCOMPARE(assembler.push(fragments.first(),0,output),i<8?ds::AssemblyResult::Pending:ds::AssemblyResult::Rejected);
        }
        QCOMPARE(assembler.bufferedBytes(),ds::PacketReassembler::MaxBytes);
        assembler.expire(200001); QCOMPARE(assembler.bufferedBytes(),0); QCOMPARE(assembler.expiredPackets(),quint64(8));
        QCOMPARE(assembler.push(first.last(),200002,output),ds::AssemblyResult::Ignored);
        auto fragments=ds::fragmentVideo(video(id,20,QByteArray(5000,'a')));
        QCOMPARE(assembler.push(fragments.first(),300000,output),ds::AssemblyResult::Pending);
        QCOMPARE(assembler.push(fragments.first(),300000,output),ds::AssemblyResult::Pending);
        auto conflicting=fragments.first(); conflicting[conflicting.size()-1]='b';
        QCOMPARE(assembler.push(conflicting,300000,output),ds::AssemblyResult::Rejected);
        QCOMPARE(assembler.bufferedBytes(),0);
        auto foreign=id; ++foreign.roomId;
        const auto wrong=ds::fragmentVideo(video(foreign,21,"data"));
        QCOMPARE(assembler.push(wrong.first(),300001,output),ds::AssemblyResult::Rejected);
    }
    void mediaWorkerOverRealUdpRecoversMissingFragment() {
        auto package=package::inspect(QString::fromUtf8(MEDIA_PLUGIN)); QVERIFY2(package.ok(),qPrintable(package.error));
        std::unique_ptr<QTemporaryDir> cache; std::unique_ptr<QLockFile> lock; QString entry;
        QVERIFY(package::extract(package,cache,entry,lock).isEmpty());
        plugins::ServiceBroker broker; broker.start(entry,cache->path(),package.info);
        const auto cleanup=qScopeGuard([&] {
            broker.stop(); QElapsedTimer wait;wait.start();while(!broker.exited()&&wait.elapsed()<10000)QTest::qWait(20);
            lock.reset();package::discardCache(cache);
        });
        QTRY_VERIFY_WITH_TIMEOUT(broker.ready()||!broker.failure().isEmpty(),80000);
        QVERIFY2(broker.ready(),qPrintable(broker.failure()));
        auto session=std::make_unique<media::MediaSession>(broker.acquire(media::SessionService,media::SessionApi));
        QVERIFY(await<bool>([&](auto*ctx,auto cb){session->open(ctx,cb);}).ok());
        media::TrackConfig config;config.kind="video";config.source="synthetic";config.preview=false;
        config.codec.codec="bgra-tiles";config.codec.bitrate=2000000;config.codec.frameRate=10;
        config.video.width=320;config.video.height=180;config.video.colorRange="full";
        const auto tx=await<media::TrackDescriptor>([&](auto*ctx,auto cb){session->createTrack(config,ctx,cb);});
        QVERIFY2(tx.ok(),qPrintable(tx.error.message));
        config.direction="receive";config.source.clear();config.preview=true;
        const auto rx=await<media::TrackDescriptor>([&](auto*ctx,auto cb){session->createTrack(config,ctx,cb);});
        QVERIFY(rx.ok());
        QUdpSocket sender,receiver;
        QVERIFY(sender.bind(QHostAddress(QHostAddress::LocalHost),quint16(0)));
        QVERIFY(receiver.bind(QHostAddress(QHostAddress::LocalHost),quint16(0)));
        receiver.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,2*1024*1024);
        const auto id=identity(); ds::PacketReassembler assembler(id); QElapsedTimer clock;clock.start();
        int frames=0,repairRequests=0,datagrams=0;bool dropNext=false,dropped=false;QImage last;
        const auto callbacks=qScopeGuard([&] {
            QObject::disconnect(&receiver,nullptr,this,nullptr);
            session->packet={};session->frame={};session->keyFrameRequired={};
        });
        session->packet=[&](media::EncodedPacket encoded) {
            auto packet=video(id,encoded.sequence,encoded.data);packet.pts=encoded.pts;packet.duration=quint32(encoded.duration);
            if(encoded.keyFrame)packet.flags|=quint16(ds::MediaFlag::KeyFrame);
            if(encoded.discontinuity)packet.flags|=quint16(ds::MediaFlag::Discontinuity);
            const auto fragments=ds::fragmentVideo(packet);QVERIFY(!fragments.isEmpty());
            for(int index=fragments.size()-1;index>=0;--index) {
                if(dropNext&&index==0){dropNext=false;dropped=true;continue;}
                const auto wire=ds::wrapZts1(fragments[index]);QVERIFY(wire.size()<=1232);
                QCOMPARE(sender.writeDatagram(wire,QHostAddress::LocalHost,receiver.localPort()),qint64(wire.size()));++datagrams;
                if(index==1)QCOMPARE(sender.writeDatagram(wire,QHostAddress::LocalHost,receiver.localPort()),qint64(wire.size()));
            }
        };
        QObject::connect(&receiver,&QUdpSocket::readyRead,this,[&] {
            while(receiver.hasPendingDatagrams()) {
                const auto datagram=receiver.receiveDatagram();
                QVERIFY(datagram.senderAddress()==QHostAddress::LocalHost);QCOMPARE(datagram.senderPort(),sender.localPort());
                QByteArray payload;QVERIFY(ds::unwrapZts1(datagram.data(),payload));ds::VideoPacket packet;
                if(assembler.push(payload,clock.nsecsElapsed()/1000,packet)!=ds::AssemblyResult::Complete)continue;
                media::EncodedPacket encoded;encoded.codec="bgra-tiles";encoded.sequence=packet.sequence;
                encoded.pts=packet.pts;encoded.duration=packet.duration;encoded.data=std::move(packet.data);
                encoded.keyFrame=(packet.flags&quint16(ds::MediaFlag::KeyFrame))!=0;
                encoded.discontinuity=(packet.flags&quint16(ds::MediaFlag::Discontinuity))!=0;
                QVERIFY(session->submitTo(rx.value,std::move(encoded)));
            }
        });
        session->keyFrameRequired=[&](const QString &track) {
            QCOMPARE(track,rx.value.id);++repairRequests;session->requestKeyFrame(tx.value.id,this);
        };
        session->frame=[&](const media::FrameView &frame) {
            if(!frame.decoded)return;++frames;
            QCOMPARE(frame.video.colorRange,QStringLiteral("full"));
            last=QImage(frame.data,frame.video.width,frame.video.height,frame.planes.first().stride,QImage::Format_ARGB32).copy();
        };
        QVERIFY(await<bool>([&](auto*ctx,auto cb){session->start(rx.value.id,ctx,cb);}).ok());
        QVERIFY(await<bool>([&](auto*ctx,auto cb){session->start(tx.value.id,ctx,cb);}).ok());
        QTRY_VERIFY_WITH_TIMEOUT(frames>=3,10000);QCOMPARE(last.size(),QSize(320,180));
        QCOMPARE(last.pixelColor(10,90),QColor(Qt::red));
        dropNext=true;QTRY_VERIFY_WITH_TIMEOUT(dropped,3000);
        QTRY_VERIFY_WITH_TIMEOUT(repairRequests>=1,5000);
        const auto before=frames;QTRY_VERIFY_WITH_TIMEOUT(frames>before+2,5000);
        const auto stats=await<media::Statistics>([&](auto*ctx,auto cb){session->statistics(ctx,cb);});QVERIFY(stats.ok());
        QVERIFY(stats.value.track(tx.value.id)["tileBaselines"].toString().toULongLong()>=2);
        QCOMPARE(stats.value.track(rx.value.id)["tileMissingPackets"].toInt(),0);
        QVERIFY(assembler.expiredPackets()>=1);
        const auto beforeRestart=frames;
        QVERIFY(await<bool>([&](auto*ctx,auto cb){session->stop(tx.value.id,ctx,cb);}).ok());
        QVERIFY(await<bool>([&](auto*ctx,auto cb){session->start(tx.value.id,ctx,cb);}).ok());
        QTRY_VERIFY_WITH_TIMEOUT(frames>beforeRestart+1,3000);
        const auto restarted=await<media::Statistics>([&](auto*ctx,auto cb){session->statistics(ctx,cb);});
        QVERIFY(restarted.ok());
        QVERIFY(restarted.value.track(tx.value.id)["tileBaselines"].toString().toULongLong()>=3);
        qInfo()<<"real UDP tiles"<<"datagrams"<<datagrams<<"frames"<<frames<<"repairRequests"<<repairRequests
               <<"expiredPackets"<<assembler.expiredPackets();
        QVERIFY(await<bool>([&](auto*ctx,auto cb){session->close(ctx,cb);}).ok());
        QTRY_VERIFY(broker.idle());
    }
};
QTEST_GUILESS_MAIN(TileUdpTests)
#include "TileUdpTests.moc"
