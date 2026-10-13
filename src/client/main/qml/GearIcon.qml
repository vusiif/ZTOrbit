import QtQuick

Canvas {
    id: gear
    property color ink: Theme.text
    implicitWidth: 24
    implicitHeight: 24
    onInkChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        ctx.translate(width / 2, height / 2)
        const scale = Math.min(width, height) / 26
        ctx.scale(scale, scale)
        ctx.strokeStyle = ink
        ctx.lineWidth = 1.7
        ctx.lineJoin = "round"
        ctx.beginPath()
        for (let i = 0; i < 8; ++i) {
            const a = i * Math.PI / 4
            for (const point of [[a - .28, 8], [a - .18, 11], [a + .18, 11], [a + .28, 8]]) {
                const x = Math.cos(point[0]) * point[1], y = Math.sin(point[0]) * point[1]
                if (i === 0 && point[0] === a - .28) ctx.moveTo(x, y)
                else ctx.lineTo(x, y)
            }
        }
        ctx.closePath()
        ctx.stroke()
        ctx.beginPath()
        ctx.arc(0, 0, 3.5, 0, Math.PI * 2)
        ctx.stroke()
    }
}
