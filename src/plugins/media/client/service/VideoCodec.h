#pragma once
#include "ExtensionRegistry.h"
#include <ztmedia/MediaTypes.h>
#include <QImage>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}
namespace vusiif::ztgui::media::engine {
class VideoCodec {
  public:
    ~VideoCodec() { close(); }
    QString open(const TrackConfig &config, const ExtensionRegistry &registry);
    void close();
    QList<EncodedPacket> encode(const QImage &image, qint64 pts, QString &error);
    QList<QPair<QImage, qint64>> decode(const EncodedPacket &packet, QString &error);
    void keyFrame() { forceKeyFrame = true; }
    bool awaitingKeyFrame() const { return waiting; }

  private:
    AVCodecContext *context = nullptr;
    SwsContext *scale = nullptr;
    ProcessorChain processors;
    TrackConfig config;
    bool forceKeyFrame = true, waiting = true;
    qint64 firstPts = -1, nextPts = 0;
    quint64 sequence = 0;
};
} // namespace vusiif::ztgui::media::engine
