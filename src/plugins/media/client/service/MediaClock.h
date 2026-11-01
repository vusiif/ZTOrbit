#pragma once
#include <ztmedia/MediaTypes.h>
#include <QMap>
#include <chrono>
extern "C" {
#include <libavutil/mathematics.h>
}
namespace vusiif::zto::media::engine {
inline qint64 monotonicTimeUs(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
class PlaybackTimeline {
public:
    static constexpr qint64 TargetDelayUs = 60000;
    qint64 schedule(const QString &group,qint64 pts,TimeBase base,qint64 now) {
        auto remote=av_rescale_q(pts,{base.numerator,base.denominator},{1,1000000});
        if(!anchors.contains(group))anchors[group]={remote,now+TargetDelayUs};
        const auto anchor=anchors[group];return anchor.second+remote-anchor.first;
    }
    void reset(const QString &group){anchors.remove(group);}
private:
    QMap<QString,QPair<qint64,qint64>> anchors;
};
}
