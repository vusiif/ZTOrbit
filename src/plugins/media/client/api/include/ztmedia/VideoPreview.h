#pragma once
#include <ztmedia/MediaTypes.h>
#include <QQuickPaintedItem>
#include <QImage>
namespace vusiif::zto::media {
// Instantiate from a page factory; no global QML registration is necessary.
class VideoPreview final : public QQuickPaintedItem {
  public:
    explicit VideoPreview(QQuickItem *parent = nullptr) : QQuickPaintedItem(parent) {}
    void present(const FrameView &frame);
    void clear() {
        image = {};
        update();
    }
    void paint(QPainter *painter) override;

  private:
    QImage image;
};
} // namespace vusiif::zto::media
