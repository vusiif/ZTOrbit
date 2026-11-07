import QtQuick

// Directional chevron used by the sidebar toggle: it points left while the
// sidebar is expanded (the click collapses it) and right while collapsed.
LineIcon {
    objectName: "chevronIcon"
    property bool pointingRight: false
    onPointingRightChanged: requestPaint()
    painter: function(ctx) {
        const direction = pointingRight ? 1 : -1
        ctx.beginPath()
        ctx.moveTo(-direction * 3.4, -6.4)
        ctx.lineTo(direction * 3.4, 0)
        ctx.lineTo(-direction * 3.4, 6.4)
        ctx.stroke()
    }
}
