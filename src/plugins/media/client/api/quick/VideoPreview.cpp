#include <ztmedia/VideoPreview.h>
#include <QPainter>
namespace vusiif::ztgui::media {
void VideoPreview::present(const FrameView &frame) {
    if (!frame.data || frame.planes.isEmpty() || frame.video.pixelFormat != "bgra")
        return;
    image = QImage(frame.data, frame.video.width, frame.video.height, frame.planes.front().stride,
                   QImage::Format_ARGB32)
                .copy();
    update();
}
void VideoPreview::paint(QPainter *painter) {
    painter->fillRect(boundingRect(), Qt::black);
    if (image.isNull())
        return;
    auto size = image.size().scaled(QSize(int(width()), int(height())), Qt::KeepAspectRatio);
    painter->drawImage(
        QRectF((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(), size.height()),
        image);
}
} // namespace vusiif::ztgui::media
