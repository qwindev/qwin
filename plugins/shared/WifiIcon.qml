import QtQuick

// WiFi strength glyph: a dot and three arcs on a Canvas, so no icon font is
// needed and colors arrive as properties. Arcs light with signal strength;
// `off` dims everything and draws a slash.
Item {
    id: icon

    property int percent: 0
    property bool off: false
    property color litColor: "white"
    property color dimColor: "#66888888"

    width: 16
    height: 16

    onPercentChanged: canvas.requestPaint()
    onOffChanged: canvas.requestPaint()
    onLitColorChanged: canvas.requestPaint()
    onDimColorChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            const cx = w / 2
            const cy = h * 0.85
            const thresholds = [10, 45, 75] // arc i lights at percent >= thresholds[i]
            ctx.lineWidth = Math.max(1.4, w / 11)
            ctx.lineCap = "round"

            ctx.fillStyle = (!icon.off && icon.percent > 0) ? icon.litColor : icon.dimColor
            ctx.beginPath()
            ctx.arc(cx, cy, ctx.lineWidth * 0.8, 0, Math.PI * 2)
            ctx.fill()

            for (let i = 0; i < 3; i++) {
                const lit = !icon.off && icon.percent >= thresholds[i]
                ctx.strokeStyle = lit ? icon.litColor : icon.dimColor
                ctx.beginPath()
                ctx.arc(cx, cy, (i + 1) * h * 0.27, -Math.PI * 0.75, -Math.PI * 0.25)
                ctx.stroke()
            }

            if (icon.off) {
                ctx.strokeStyle = icon.dimColor
                ctx.beginPath()
                ctx.moveTo(w * 0.15, h * 0.08)
                ctx.lineTo(w * 0.85, h * 0.92)
                ctx.stroke()
            }
        }
    }
}
