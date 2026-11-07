import QtQuick

// ZeroTier menu motif: one accent-coloured hub connected to three leaf nodes.
LineIcon {
    objectName: "zeroTierMenuIcon"
    painter: function(ctx) {
        const nodes = [[0, -6.6], [-6.4, 4.2], [6.4, 4.2]]
        const hub = 2.1, leaf = 2.4
        ctx.beginPath()
        for (const node of nodes) {
            const distance = Math.sqrt(node[0] * node[0] + node[1] * node[1])
            const nx = node[0] / distance, ny = node[1] / distance
            ctx.moveTo(nx * (hub + 1.1), ny * (hub + 1.1))
            ctx.lineTo(nx * (distance - leaf - 0.9), ny * (distance - leaf - 0.9))
        }
        ctx.stroke()
        for (const node of nodes) {
            ctx.beginPath()
            ctx.arc(node[0], node[1], leaf, 0, Math.PI * 2)
            ctx.stroke()
        }
        ctx.fillStyle = accentInk
        ctx.beginPath()
        ctx.arc(0, 0, hub, 0, Math.PI * 2)
        ctx.fill()
    }
}
