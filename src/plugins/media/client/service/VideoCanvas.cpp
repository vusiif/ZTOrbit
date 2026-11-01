#include "VideoCanvas.h"
#include <QPainter>
namespace vusiif::zto::media::engine {
QImage composeVideoCanvas(const QImage &source, int width, int height) {
    if (width < 2 || height < 2)
        return {};
    if (source.isNull() || source.width() < 1 || source.height() < 1) {
        QImage blank(width, height, QImage::Format_ARGB32);
        blank.fill(Qt::black);
        return blank;
    }
    if (source.width() == width && source.height() == height)
        return source.convertToFormat(QImage::Format_ARGB32);
    QImage canvas(width, height, QImage::Format_ARGB32);
    canvas.fill(Qt::black);
    const auto scaled = source.scaled(width, height, Qt::KeepAspectRatio, Qt::FastTransformation);
    if (scaled.isNull())
        return canvas;
    QPainter painter(&canvas);
    painter.drawImage((width - scaled.width()) / 2, (height - scaled.height()) / 2, scaled);
    painter.end();
    return canvas;
}
} // namespace vusiif::zto::media::engine
