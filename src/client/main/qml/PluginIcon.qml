import QtQuick

// Unified fallback for plugin menus that do not ship their own icon: four
// modules in a 2×2 grid, drawn with the same single stroke as the other icons.
LineIcon {
    objectName: "pluginFallbackIcon"
    painter: function(ctx) {
        const half = 3.1, radius = 1.2
        const centers = [[-3.5, -3.5], [3.5, -3.5], [-3.5, 3.5], [3.5, 3.5]]
        for (const center of centers) {
            const x = center[0] - half, y = center[1] - half, size = half * 2
            ctx.beginPath()
            ctx.moveTo(x + radius, y)
            ctx.lineTo(x + size - radius, y)
            ctx.quadraticCurveTo(x + size, y, x + size, y + radius)
            ctx.lineTo(x + size, y + size - radius)
            ctx.quadraticCurveTo(x + size, y + size, x + size - radius, y + size)
            ctx.lineTo(x + radius, y + size)
            ctx.quadraticCurveTo(x, y + size, x, y + size - radius)
            ctx.lineTo(x, y + radius)
            ctx.quadraticCurveTo(x, y, x + radius, y)
            ctx.closePath()
            ctx.stroke()
        }
    }
}
