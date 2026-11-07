import QtQuick

// Shared surface for the 24 px line-icon family. Concrete icons assign the
// `painter` function, which draws in a 24×24 coordinate space centred on
// (0, 0). The stroke keeps its nominal pixel width at 20 px and 24 px sizes,
// so navigation icons and toolbar icons share the same visual weight.
Canvas {
    id: icon
    property color ink: Theme.text
    property color accentInk: Theme.accent
    property real strokeWidth: 1.7
    // function(ctx) drawing the icon with the context already translated and
    // scaled to the 24 px design space.
    property var painter: null
    implicitWidth: 24
    implicitHeight: 24
    onInkChanged: requestPaint()
    onAccentInkChanged: requestPaint()
    onStrokeWidthChanged: requestPaint()
    onPainterChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const scale = Math.min(width, height) / 24
        if (!(scale > 0)) return
        ctx.translate(width / 2, height / 2)
        ctx.scale(scale, scale)
        ctx.strokeStyle = ink
        ctx.fillStyle = ink
        ctx.lineWidth = strokeWidth / scale
        ctx.lineCap = "round"
        ctx.lineJoin = "round"
        if (painter) painter(ctx)
    }
}
