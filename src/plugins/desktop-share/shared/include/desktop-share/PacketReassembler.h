#pragma once
#include <desktop-share/MediaProtocol.h>
#include <QBitArray>
#include <QMap>
#include <QQueue>
#include <QSet>

namespace vusiif::zto::desktop_share {
enum class AssemblyResult { Pending, Complete, Ignored, Rejected };
// A receiver owns one instance per negotiated media identity. It must also
// check the UDP peer against the selected server before calling push().
class PacketReassembler {
public:
    static constexpr int MaxPackets=8, MaxBytes=8*1024*1024;
    static constexpr qint64 LifetimeUs=200000;
    explicit PacketReassembler(MediaHeader expected) : expected(std::move(expected)) {}
    AssemblyResult push(QByteArrayView payload,qint64 now,VideoPacket &packet);
    void expire(qint64 now);
    int bufferedBytes() const { return reserved; }
    int pendingPackets() const { return pending.size(); }
    int peakPendingPackets() const { return peakPending; }
    int peakBufferedBytes() const { return peakReserved; }
    quint64 expiredPackets() const { return expired; }
    // Header of the most recently decoded datagram, valid after push() returns
    // Pending or Complete. The ordered stream layer uses the sequence of an
    // incomplete packet to start its whole-packet-loss deadline.
    const MediaHeader &lastHeader() const { return latest; }
private:
    struct Pending { MediaHeader header; QByteArray bytes; QBitArray fragments; int count=0; qint64 created=0; };
    MediaHeader expected;
    MediaHeader latest;
    QMap<quint64,Pending> pending;
    QSet<quint64> retired;
    QQueue<quint64> order;
    int reserved=0;
    int peakReserved=0;
    int peakPending=0;
    quint64 highest=0, expired=0;
    void retire(quint64 sequence);
};
} // namespace vusiif::zto::desktop_share
