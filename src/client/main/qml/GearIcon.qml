import QtQuick

// Settings gear, drawn with the shared 24 px line-icon surface. Coordinates are
// the former standalone gear scaled from its 26 px box to the family box.
LineIcon {
    objectName: "gearIcon"
    painter: function(ctx) {
        ctx.beginPath()
        for (let i = 0; i < 8; ++i) {
            const a = i * Math.PI / 4
            const points = [[a - .28, 7.4], [a - .18, 10.15], [a + .18, 10.15], [a + .28, 7.4]]
            for (let p = 0; p < points.length; ++p) {
                const x = Math.cos(points[p][0]) * points[p][1]
                const y = Math.sin(points[p][0]) * points[p][1]
                if (i === 0 && p === 0) ctx.moveTo(x, y)
                else ctx.lineTo(x, y)
            }
        }
        ctx.closePath()
        ctx.stroke()
        ctx.beginPath()
        ctx.arc(0, 0, 3.2, 0, Math.PI * 2)
        ctx.stroke()
    }
}
