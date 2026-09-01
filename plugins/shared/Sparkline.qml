import QtQuick

// Line+area chart for a rolling series, in the same Canvas idiom as
// WifiIcon: colors arrive as properties (shared/ components stay
// palette-agnostic) and every change calls requestPaint(), since Canvas does
// not repaint on binding changes.
//
// `values` is oldest-first, so the line reads left to right. `maxValue`
// fixes the vertical scale; 0 auto-scales to the window's own peak, floored
// by `minScale` so an idle series does not magnify stray bytes.
Item {
    id: spark

    property var values: []
    property real maxValue: 0
    property real minScale: 0
    property color lineColor: "white"
    property color fillColor: "#33FFFFFF"
    property real lineWidth: 1.5

    width: 46
    height: 16

    onValuesChanged: canvas.requestPaint()
    onMaxValueChanged: canvas.requestPaint()
    onMinScaleChanged: canvas.requestPaint()
    onLineColorChanged: canvas.requestPaint()
    onFillColorChanged: canvas.requestPaint()
    onLineWidthChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height

            // Normalize degenerate inputs into a plottable 2+ point series
            // rather than special-case the draw path below.
            const raw = spark.values || []
            let pts
            if (raw.length === 0)
                pts = [0, 0]
            else if (raw.length === 1)
                pts = [raw[0], raw[0]]
            else
                pts = raw

            const n = pts.length // >= 2 from here on

            let scale = spark.maxValue
            if (scale <= 0) {
                let peak = 0
                for (let i = 0; i < n; i++)
                    if (pts[i] > peak) peak = pts[i]
                scale = Math.max(peak, spark.minScale)
            }
            // An all-zero series with no floor leaves scale at 0; 1 makes the
            // division a flat baseline instead of NaN, which paints nothing.
            if (scale <= 0) scale = 1

            ctx.lineWidth = spark.lineWidth
            ctx.lineJoin = "round"
            ctx.lineCap = "round"

            // Half a stroke width, so peak and baseline are never clipped.
            const inset = ctx.lineWidth / 2
            const usable = Math.max(h - 2 * inset, 1)
            const baseline = h - inset

            function xAt(i) { return (i / (n - 1)) * w }
            function yAt(v) {
                const t = Math.max(0, Math.min(1, v / scale))
                return baseline - t * usable
            }

            // The area under the line.
            ctx.beginPath()
            ctx.moveTo(xAt(0), baseline)
            for (let i = 0; i < n; i++)
                ctx.lineTo(xAt(i), yAt(pts[i]))
            ctx.lineTo(xAt(n - 1), baseline)
            ctx.closePath()
            ctx.fillStyle = spark.fillColor
            ctx.fill()

            // The line, over the fill.
            ctx.beginPath()
            ctx.moveTo(xAt(0), yAt(pts[0]))
            for (let i = 1; i < n; i++)
                ctx.lineTo(xAt(i), yAt(pts[i]))
            ctx.strokeStyle = spark.lineColor
            ctx.stroke()
        }
    }
}
