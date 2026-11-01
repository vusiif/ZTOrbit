#include "TileCodec.h"
#include <QtTest>
#include <QtEndian>
using namespace vusiif::zto;
using namespace media::engine;
namespace {
media::TrackConfig config(int width=128,int height=128,int bitrate=2000000) {
    media::TrackConfig c; c.kind="video"; c.codec.codec="bgra-tiles";
    c.video.width=width; c.video.height=height; c.video.colorRange="full"; c.codec.bitrate=bitrate;
    return c;
}
QImage noise(int width,int height,int offset=0) {
    QImage image(width,height,QImage::Format_ARGB32);
    quint32 state=0x9e3779b9u+quint32(offset);
    for (int y=0;y<height;++y) for(int x=0;x<width;++x) {
        state^=state<<13;state^=state>>17;state^=state<<5;
        image.setPixel(x,y,0xff000000u|(state&0xffffffu));
    }
    return image;
}
}
class TileCodecTests : public QObject {
    Q_OBJECT
private slots:
    void lateReceiverGetsOneCoalescedFollowupWithoutAbandoningCurrentSweep() {
        const auto c=config(256,256,2000000);
        TileEncoder encoder(c);TileDecoder decoder(c);QString error;
        const auto image=noise(256,256);encoder.capture(image,0);
        const auto lostBeforeNegotiation=encoder.drain(0);QCOMPARE(lostBeforeNegotiation.size(),1);
        // Several tracks/viewers become ready after codec packet zero was
        // already emitted. Their requests must neither restart nor disappear.
        for(int i=0;i<20;++i) encoder.keyFrame();
        QImage output;quint64 previousEpoch=1;bool sawFirstSweepFinish=false;
        for(qint64 now=5000;now<3000000;now+=5000) for(const auto &p:encoder.drain(now)) {
            const auto currentEpoch=qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(p.data.constData()+16));
            if(currentEpoch==2 && previousEpoch==1) sawFirstSweepFinish=true;
            previousEpoch=currentEpoch;
            const auto frames=decoder.decode(p,now,error);QVERIFY2(error.isEmpty(),qPrintable(error));
            if(!frames.isEmpty()) output=frames.last().first;
        }
        QVERIFY(sawFirstSweepFinish);QCOMPARE(output,image);
        QCOMPARE(encoder.statistics()["tileBaselines"].toString(),QStringLiteral("2"));
    }
    void repeatedRowsUseBoundedLosslessDeflate() {
        const auto c=config(1920,1080,4000000);
        TileEncoder encoder(c);TileDecoder decoder(c);QString error;
        QImage image(1920,1080,QImage::Format_ARGB32);
        for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x)
            image.setPixel(x,y,qRgb(x%256,(x*37)%256,(x*71)%256));
        encoder.capture(image,0);QImage output;int packets=0;quint64 bytes=0;
        for(qint64 now=0;now<3000000 && output.isNull();now+=5000)
            for(const auto &p:encoder.drain(now)) {
                ++packets;bytes+=p.data.size();
                const auto frames=decoder.decode(p,now,error);
                QVERIFY2(error.isEmpty(),qPrintable(error));
                if(!frames.isEmpty()) output=frames.last().first;
            }
        QCOMPARE(output,image);QVERIFY(packets<30);
        QVERIFY(bytes<quint64(image.sizeInBytes())/16);
    }
    void deflateRejectsBombTruncationAndTrailingInput() {
        const auto c=config(64,64);TileEncoder encoder(c);QString error;
        QImage image(64,64,QImage::Format_ARGB32);image.fill(Qt::white);
        encoder.capture(image,0);const auto original=encoder.drain(0).first();
        const auto test=[&](const QByteArray &blob) {
            auto p=original;p.data.resize(TileEncoder::HeaderBytes+8);
            qToBigEndian<quint16>(2,reinterpret_cast<uchar*>(p.data.data()+42));
            qToBigEndian<quint32>(quint32(blob.size()),reinterpret_cast<uchar*>(p.data.data()+44));
            p.data+=blob;TileDecoder decoder(c);error.clear();
            QVERIFY(decoder.decode(p,0,error).isEmpty());QVERIFY(!error.isEmpty());
            QCOMPARE(decoder.statistics()["tileCoverage"].toInt(),0);
        };
        const auto compressed=qCompress(QByteArray(64*64*4,'x'),1).mid(4);
        test(compressed.first(compressed.size()-1));test(compressed+QByteArray(1,'x'));
        test(qCompress(QByteArray(2*64*64*4,'x'),1).mid(4));
    }
    void complementaryLossAcrossRefreshesCompletesOnlyVerifiedCanvas() {
        const auto c=config(256,256,2000000);
        TileEncoder encoder(c); TileDecoder decoder(c); QString error;
        const auto original=noise(256,256); encoder.capture(original,0);
        QImage output;
        media::EncodedPacket old;
        for(int sweep=0;sweep<2;++sweep) {
            if(sweep) encoder.keyFrame();
            int index=0;
            for(qint64 now=sweep*2000000;now<(sweep+1)*2000000;now+=5000) {
                for(const auto &p:encoder.drain(now)) {
                    if(!p.keyFrame) continue;
                    if(!sweep && old.data.isEmpty()) old=p;
                    if((index++ % 2) != sweep) continue;
                    const auto frames=decoder.decode(p,now,error);
                    QVERIFY2(error.isEmpty(),qPrintable(error));
                    if(!frames.isEmpty()) output=frames.last().first;
                }
            }
            if(!sweep) {QVERIFY(output.isNull());QCOMPARE(decoder.statistics()["tileCoverage"].toInt(),8);}
        }
        QCOMPARE(output,original);
        QVERIFY(decoder.decode(old,5000000,error).isEmpty());
        TileDecoder newStream(c);
        QVERIFY(newStream.decode(old,5000000,error).isEmpty());
        QCOMPARE(newStream.statistics()["tileCoverage"].toInt(),1);
    }
    void forwarded1080BaselineUsesBoundedBurstsAndSurvivesRefresh() {
        auto c=config(1920,1080,4000000);
        TileEncoder encoder(c);TileDecoder decoder(c);QString error;
        QImage original(1920,1080,QImage::Format_ARGB32);original.fill(QColor(230,30,200));
        encoder.capture(original,0);
        int frames=0;quint64 bytes=0;
        for(qint64 now=0;now<3000000;now+=10000) {
            if(now==1000000 || now==2000000) encoder.keyFrame();
            for(const auto &packet:encoder.drain(now)) {
                QVERIFY(packet.data.size()<=8*1024);
                bytes+=packet.data.size();
                const auto decoded=decoder.decode(packet,now,error);
                QVERIFY2(error.isEmpty(),qPrintable(error));
                if(!decoded.isEmpty()) { QCOMPARE(decoded.last().first,original);++frames; }
            }
            QVERIFY(bytes<=125000+quint64(now/1000000.0*500000));
            if(now==900000) QVERIFY(frames>0);
        }
        QVERIFY(frames>=3);
        QVERIFY(!decoder.refreshRequired(3000000));
    }
    void smallFanoutBurstsDoNotStarveIncompressibleTiles() {
        const auto c=config(256,256,2000000);
        TileEncoder encoder(c); TileDecoder decoder(c); QString error;
        const auto original=noise(256,256); encoder.capture(original,0);
        QImage output;
        for(qint64 now=0;now<3000000 && output.isNull();now+=5000) {
            for(const auto &p:encoder.drain(now)) {
                QVERIFY(p.data.size()<=64*64*4+TileEncoder::HeaderBytes+8);
                const auto decoded=decoder.decode(p,now,error);
                QVERIFY2(error.isEmpty(),qPrintable(error));
                if(!decoded.isEmpty()) output=decoded.last().first;
            }
        }
        QCOMPARE(output,original);
    }
    void completeBaselineAndStaticSuppression_data() {
        QTest::addColumn<int>("width"); QTest::addColumn<int>("height");
        QTest::newRow("720p")<<1280<<720; QTest::newRow("1080p")<<1920<<1080;
    }
    void completeBaselineAndStaticSuppression() {
        QFETCH(int,width); QFETCH(int,height);
        auto c=config(width,height); TileEncoder encoder(c); TileDecoder decoder(c);
        QImage original(width,height,QImage::Format_ARGB32); original.fill(Qt::white);
        original.setPixelColor(width-1,height-1,Qt::red);
        encoder.capture(original,123456);
        QImage output; QString error;
        for(qint64 now=0;now<3000000 && output.isNull();now+=5000)
            for(const auto &packet:encoder.drain(now)) {
                QVERIFY(packet.data.size()<=TileEncoder::PacketBytes);
                const auto frames=decoder.decode(packet,now,error); QVERIFY2(error.isEmpty(),qPrintable(error));
                if(!frames.isEmpty()) output=frames.last().first;
            }
        QCOMPARE(output,original);
        const auto before=encoder.statistics()["tilesSent"];
        encoder.capture(original,2000000);
        const auto packets=encoder.drain(2000000);
        for(const auto &packet:packets) QCOMPARE(packet.data.size(),qsizetype(TileEncoder::HeaderBytes));
        QCOMPARE(encoder.statistics()["tilesSent"],before);
    }
    void onlyChangedTilesAndLatestPixels() {
        auto c=config(); TileEncoder encoder(c); TileDecoder decoder(c); QString error;
        QImage original(128,128,QImage::Format_ARGB32); original.fill(Qt::white);
        encoder.capture(original,0); const auto initial=encoder.drain(0); QVERIFY(!initial.isEmpty());
        QVERIFY(!decoder.decode(initial.first(),0,error).isEmpty());
        auto updated=original.copy(); updated.setPixelColor(70,5,Qt::red);
        encoder.capture(updated,100000); const auto packets=encoder.drain(100000); QCOMPARE(packets.size(),1);
        QCOMPARE(qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(packets.first().data.constData()+12)),quint16(1));
        const auto frames=decoder.decode(packets.first(),100000,error); QVERIFY(!frames.isEmpty());
        QCOMPARE(frames.last().first,updated);
        qInfo()<<"lossless desktop payload bytes"<<"baseline"<<initial.first().data.size()
               <<"one changed block"<<packets.first().data.size()<<"raw frame"<<original.sizeInBytes();
        const auto sent=encoder.statistics()["tilesSent"];
        auto transient=updated.copy(); transient.setPixelColor(2,2,Qt::green);
        encoder.capture(transient,110000); encoder.capture(updated,120000);
        encoder.drain(120000); QCOMPARE(encoder.statistics()["tilesSent"],sent);
    }
    void boundedBudgetAndFairRotation() {
        auto c=config(256,256,64000); TileEncoder encoder(c); TileDecoder decoder(c); QString error;
        const auto original=noise(256,256); encoder.capture(original,0);
        quint64 bytes=0; QImage output;
        for(qint64 now=0;now<40000000 && output.isNull();now+=100000) {
            for(const auto &packet:encoder.drain(now)) {
                bytes+=packet.data.size();
                const auto frames=decoder.decode(packet,now,error); QVERIFY2(error.isEmpty(),qPrintable(error));
                if(!frames.isEmpty()) output=frames.last().first;
            }
            QVERIFY(bytes<=32768+quint64(now/1000000.0*8000)+1);
        }
        QCOMPARE(output,original);
        QCOMPARE(encoder.statistics()["tilePending"].toInt(),0);
        QVERIFY(encoder.statistics()["tileBudgetDeferrals"].toString().toULongLong()>0);
    }
    void reorderingLossAndBaselineRepair() {
        auto c=config(128,128,64000); TileEncoder encoder(c); TileDecoder decoder(c); QString error;
        auto current=noise(128,128); encoder.capture(current,0);
        QList<media::EncodedPacket> packets;
        for(qint64 now=0;now<10000000;now+=2500000) packets+=encoder.drain(now);
        QVERIFY(packets.size()>=4); QImage output;
        for(auto it=packets.crbegin();it!=packets.crend();++it) {
            const auto frames=decoder.decode(*it,10000000,error); QVERIFY2(error.isEmpty(),qPrintable(error));
            if(!frames.isEmpty()) output=frames.last().first;
        }
        QCOMPARE(output,current); QVERIFY(!decoder.refreshRequired(10200001));
        QVERIFY(decoder.decode(packets.first(),10200001,error).isEmpty());
        current.setPixelColor(1,1,Qt::red); encoder.capture(current,11000000);
        const auto dropped=encoder.drain(11000000); QVERIFY(!dropped.isEmpty());
        const auto heartbeat=encoder.drain(12000000); QVERIFY(!heartbeat.isEmpty());
        for(const auto &packet:heartbeat) decoder.decode(packet,12000000,error);
        QVERIFY(decoder.refreshRequired(12200001));
        encoder.keyFrame();
        output={};
        for(qint64 now=13000000;now<24000000 && output.isNull();now+=2500000)
            for(const auto &packet:encoder.drain(now)) {
                const auto frames=decoder.decode(packet,now,error); QVERIFY2(error.isEmpty(),qPrintable(error));
                if(!frames.isEmpty()) output=frames.last().first;
            }
        QCOMPARE(output,current); QVERIFY(!decoder.refreshRequired(25000000));
        QVERIFY(decoder.decode(packets.last(),25000000,error).isEmpty());
    }
    void malformedPacketsDoNotProducePartialPictures() {
        auto c=config(); TileEncoder encoder(c); TileDecoder decoder(c); QString error;
        QImage image(128,128,QImage::Format_ARGB32); image.fill(Qt::white); encoder.capture(image,0);
        auto packet=encoder.drain(0).first(); packet.data[14]=1;
        QVERIFY(decoder.decode(packet,0,error).isEmpty()); QVERIFY(!error.isEmpty());
        QVERIFY(decoder.refreshRequired(0));
        error.clear(); packet=encoder.drain(1000000).first(); packet.data=QByteArray(300000,'x');
        QVERIFY(decoder.decode(packet,1000000,error).isEmpty()); QVERIFY(!error.isEmpty());
        QCOMPARE(decoder.statistics()["tileCoverage"].toInt(),0);
    }
    void outputCadenceIsBounded() {
        auto c=config(64,64); TileEncoder encoder(c); int packets=0;
        QImage image(64,64,QImage::Format_ARGB32);
        for(qint64 now=0;now<1000000;now+=1000) {
            image.fill(qRgb(int(now/1000)%256,20,40)); encoder.capture(image,now);
            packets+=encoder.drain(now).size();
        }
        QVERIFY(packets>=25); QVERIFY(packets<=31);
    }
    void envelopeSequenceDoesNotChangeCodecOrdering() {
        auto c=config(); TileEncoder encoder(c); TileDecoder decoder(c);QString error;
        QImage image(128,128,QImage::Format_ARGB32);image.fill(Qt::white);encoder.capture(image,0);
        auto first=encoder.drain(0).first();first.sequence=100;
        QVERIFY(!decoder.decode(first,0,error).isEmpty());
        QVERIFY(!decoder.refreshRequired(300000));
        image.setPixelColor(1,1,Qt::red);encoder.capture(image,500000);
        auto next=encoder.drain(500000).first();next.sequence=101;
        const auto frames=decoder.decode(next,500000,error);QVERIFY(!frames.isEmpty());
        QCOMPARE(frames.last().first,image);QVERIFY(!decoder.refreshRequired(800000));
    }
    void fixedWireGoldenIsLossless() {
        // Independent ZTD1 v1 golden: a 16x16 red baseline, two PackBits
        // runs of 128 BGRA pixels. Locks byte order, offsets and run encoding.
        const auto golden=QByteArray::fromHex(
            "5a544431010100400010001000010000"
            "000000000000000100000000000000000000000000000000"
            "000000010000000aff0000ffffff0000ffff");
        auto c=config(16,16);TileEncoder encoder(c);TileDecoder decoder(c);
        QImage image(16,16,QImage::Format_ARGB32);image.fill(Qt::red);
        encoder.capture(image,123456);const auto packet=encoder.drain(0).first();
        QCOMPARE(packet.data,golden);
        media::EncodedPacket external;external.codec="bgra-tiles";external.keyFrame=true;
        external.sequence=99;external.pts=123456;external.data=golden;QString error;
        const auto frames=decoder.decode(external,0,error);QVERIFY2(error.isEmpty(),qPrintable(error));
        QVERIFY(!frames.isEmpty());QCOMPARE(frames.first().first,image);
        QCOMPARE(frames.first().second,qint64(123456));
    }
    void senderRestartKeepsReceiverAndBudgetHistory() {
        auto c=config();TileEncoder encoder(c);TileDecoder decoder(c);QString error;
        QImage image(128,128,QImage::Format_ARGB32);image.fill(Qt::white);encoder.capture(image,0);
        const auto before=encoder.drain(0).first();QVERIFY(!decoder.decode(before,0,error).isEmpty());
        encoder.stop();QVERIFY(encoder.drain(50000).isEmpty());
        image.fill(Qt::blue);encoder.capture(image,100000);
        const auto after=encoder.drain(100000).first();QVERIFY(after.sequence>before.sequence);
        const auto frames=decoder.decode(after,100000,error);QVERIFY2(error.isEmpty(),qPrintable(error));
        QVERIFY(!frames.isEmpty());QCOMPARE(frames.last().first,image);
        QCOMPARE(encoder.statistics()["tileBaselines"].toString(),QStringLiteral("2"));
    }
};
QTEST_GUILESS_MAIN(TileCodecTests)
#include "TileCodecTests.moc"
