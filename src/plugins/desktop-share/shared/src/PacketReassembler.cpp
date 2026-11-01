#include <desktop-share/PacketReassembler.h>
#include <cstring>
#include <algorithm>
#include <QDebug>

namespace vusiif::zto::desktop_share {
void PacketReassembler::retire(quint64 sequence) {
    if(retired.contains(sequence)) return;
    retired.insert(sequence); order.enqueue(sequence);
    while(order.size()>1024) retired.remove(order.dequeue());
}
void PacketReassembler::expire(qint64 now) {
    for(auto it=pending.begin();it!=pending.end();) {
        if(now-it->created<LifetimeUs) { ++it; continue; }
        if (qEnvironmentVariableIntValue("ZTO_DESKTOP_SHARE_DIAGNOSTICS"))
            qInfo() << "zds fragment expiry" << it.key() << "received" << it->count
                    << "required" << it->header.fragmentCount << "bytes" << it->header.packetBytes;
        reserved-=it->bytes.size(); retire(it.key()); ++expired; it=pending.erase(it);
    }
}
AssemblyResult PacketReassembler::push(QByteArrayView payload,qint64 now,VideoPacket &packet) {
    expire(now);
    DecodedDatagram decoded;
    if(!decodeDatagram(payload,decoded)) return AssemblyResult::Rejected;
    const auto &h=decoded.header;
    latest=h;
    if(h.type!=MediaType::Video || h.token!=expected.token || h.roomId!=expected.roomId ||
       h.sessionId!=expected.sessionId || h.streamEpoch!=expected.streamEpoch) return AssemblyResult::Rejected;
    if(retired.contains(h.sequence) || (highest>h.sequence && highest-h.sequence>=1024)) return AssemblyResult::Ignored;
    highest=std::max(highest,h.sequence);
    auto it=pending.find(h.sequence);
    if(it==pending.end()) {
        if(pending.size()>=MaxPackets || h.packetBytes>quint32(MaxBytes-reserved)) {
            retire(h.sequence); return AssemblyResult::Rejected;
        }
        Pending value; value.header=h; value.bytes.resize(h.packetBytes);
        value.fragments=QBitArray(h.fragmentCount); value.created=now;
        reserved+=value.bytes.size(); it=pending.insert(h.sequence,std::move(value));
        peakReserved=std::max(peakReserved,reserved);
        peakPending=std::max(peakPending,int(pending.size()));
    }
    const auto fail=[&] {
        reserved-=it->bytes.size(); pending.erase(it); retire(h.sequence); return AssemblyResult::Rejected;
    };
    if(!sameFragmentMetadata(it->header,h)) return fail();
    const int offset=h.fragmentIndex*MaxFragmentBytes;
    if(it->fragments.testBit(h.fragmentIndex)) {
        if(std::memcmp(it->bytes.constData()+offset,decoded.fragment.constData(),h.fragmentBytes)) return fail();
        return AssemblyResult::Pending;
    }
    std::memcpy(it->bytes.data()+offset,decoded.fragment.constData(),h.fragmentBytes);
    it->fragments.setBit(h.fragmentIndex); ++it->count;
    if(it->count!=h.fragmentCount) return AssemblyResult::Pending;
    packet={h.flags,h.token,h.sessionId,h.roomId,h.streamEpoch,h.sequence,h.pts,h.dts,h.duration,std::move(it->bytes)};
    reserved-=h.packetBytes; pending.erase(it); retire(h.sequence);
    return AssemblyResult::Complete;
}
} // namespace vusiif::zto::desktop_share
