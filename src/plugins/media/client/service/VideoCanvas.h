#pragma once
#include <QImage>
namespace vusiif::zto::media::engine {
// Compose one captured frame onto the fixed BGRA canvas used by both the H.264
// encoder and the local preview. The canvas is exactly width x height (the
// validated even track format, enforced by the session config validation); the
// source keeps its aspect ratio, is centered, and the remaining area is black.
// A source that already has the canvas size is only converted to ARGB32.
QImage composeVideoCanvas(const QImage &source, int width, int height);
} // namespace vusiif::zto::media::engine
