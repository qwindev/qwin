import QtQuick

// Weather glyph drawn from a WMO weather_code, the codes Open-Meteo returns
// (labelFor below has the group boundaries). `day` swaps the clear-sky glyph
// for a crescent at night; every other group looks the same either way.
// Colors are passed in, as shared/ components stay palette-agnostic:
// `litColor` draws the foreground shape, `dimColor` whatever sits behind or
// beside it (a second cloud layer, alternating fog bands).
Item {
    id: icon

    property int code: 0
    property bool day: true
    property color litColor: "white"
    property color dimColor: "#66888888"

    width: 16
    height: 16

    onCodeChanged: canvas.requestPaint()
    onDayChanged: canvas.requestPaint()
    onLitColorChanged: canvas.requestPaint()
    onDimColorChanged: canvas.requestPaint()

    // Human label for a weather_code. Here rather than in the weather plugin
    // so the bar, popup header and forecast rows share one copy of the
    // mapping; call it on any instance, it reads no properties of its own.
    function labelFor(code) {
        if (code === 0)
            return "Clear"
        if (code >= 1 && code <= 2)
            return "Partly cloudy"
        if (code === 3)
            return "Overcast"
        if (code === 45 || code === 48)
            return "Fog"
        if (code >= 51 && code <= 57)
            return "Drizzle"
        if (code >= 61 && code <= 67)
            return "Rain"
        if (code >= 71 && code <= 77)
            return "Snow"
        if (code >= 80 && code <= 82)
            return "Rain showers"
        if (code === 85 || code === 86)
            return "Snow showers"
        if (code >= 95 && code <= 99)
            return "Thunderstorm"
        return "Unknown"
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            const cx = w / 2
            const s = Math.min(w, h) // the scale unit for every shape below
            const lit = icon.litColor
            const dim = icon.dimColor
            const code = icon.code

            ctx.lineCap = "round"
            ctx.lineJoin = "round"

            function drawSun(x, y, r, color) {
                ctx.fillStyle = color
                ctx.beginPath()
                ctx.arc(x, y, r, 0, Math.PI * 2)
                ctx.fill()
                ctx.strokeStyle = color
                ctx.lineWidth = Math.max(1, s / 12)
                for (let i = 0; i < 8; i++) {
                    const a = i * Math.PI / 4
                    ctx.beginPath()
                    ctx.moveTo(x + Math.cos(a) * r * 1.4, y + Math.sin(a) * r * 1.4)
                    ctx.lineTo(x + Math.cos(a) * r * 1.9, y + Math.sin(a) * r * 1.9)
                    ctx.stroke()
                }
            }

            // A full disc with a shifted circle punched out (destination-out):
            // the simplest crescent on a transparent Canvas, which has no
            // background color to clip against.
            function drawMoon(x, y, r, color) {
                ctx.save()
                ctx.fillStyle = color
                ctx.beginPath()
                ctx.arc(x, y, r, 0, Math.PI * 2)
                ctx.fill()
                ctx.globalCompositeOperation = "destination-out"
                ctx.beginPath()
                ctx.arc(x + r * 0.55, y - r * 0.4, r * 0.85, 0, Math.PI * 2)
                ctx.fill()
                ctx.restore()
            }

            // Three overlapping circles and a base rect filled as one path;
            // the overlaps are harmless under the nonzero fill rule, since
            // every sub-path winds the same way.
            function drawCloud(x, y, cs, color) {
                ctx.fillStyle = color
                ctx.beginPath()
                ctx.arc(x - cs * 0.85, y, cs * 0.55, 0, Math.PI * 2)
                ctx.arc(x - cs * 0.15, y - cs * 0.4, cs * 0.7, 0, Math.PI * 2)
                ctx.arc(x + cs * 0.55, y - cs * 0.05, cs * 0.55, 0, Math.PI * 2)
                ctx.rect(x - cs * 0.85, y - cs * 0.05, cs * 1.65, cs * 0.6)
                ctx.fill()
            }

            function drawDrops(x, y, cs, color, count, length) {
                ctx.strokeStyle = color
                ctx.lineWidth = Math.max(1, s / 9)
                for (let i = 0; i < count; i++) {
                    const dx = x - cs * 0.55 + i * (cs * 1.1 / (count - 1))
                    ctx.beginPath()
                    ctx.moveTo(dx, y)
                    ctx.lineTo(dx - length * 0.35, y + length)
                    ctx.stroke()
                }
            }

            function drawFlakes(x, y, cs, color, count) {
                ctx.strokeStyle = color
                ctx.lineWidth = Math.max(1, s / 11)
                const r = s * 0.09
                for (let i = 0; i < count; i++) {
                    const dx = x - cs * 0.55 + i * (cs * 1.1 / (count - 1))
                    const dy = y + s * 0.16
                    for (let a = 0; a < 3; a++) {
                        const ang = a * Math.PI / 3
                        ctx.beginPath()
                        ctx.moveTo(dx - Math.cos(ang) * r, dy - Math.sin(ang) * r)
                        ctx.lineTo(dx + Math.cos(ang) * r, dy + Math.sin(ang) * r)
                        ctx.stroke()
                    }
                }
            }

            function drawBolt(x, y, cs, color) {
                ctx.fillStyle = color
                ctx.beginPath()
                ctx.moveTo(x + cs * 0.05, y)
                ctx.lineTo(x - cs * 0.28, y + cs * 0.55)
                ctx.lineTo(x - cs * 0.02, y + cs * 0.55)
                ctx.lineTo(x - cs * 0.22, y + cs * 1.05)
                ctx.lineTo(x + cs * 0.32, y + cs * 0.35)
                ctx.lineTo(x + cs * 0.05, y + cs * 0.35)
                ctx.closePath()
                ctx.fill()
            }

            function drawFog(color, altColor) {
                ctx.lineWidth = Math.max(1, s / 9)
                const rows = [
                    { y: h * 0.38, hw: w * 0.34, c: altColor },
                    { y: h * 0.58, hw: w * 0.42, c: color },
                    { y: h * 0.78, hw: w * 0.28, c: altColor }
                ]
                for (const row of rows) {
                    ctx.strokeStyle = row.c
                    ctx.beginPath()
                    ctx.moveTo(cx - row.hw, row.y)
                    ctx.lineTo(cx + row.hw, row.y)
                    ctx.stroke()
                }
            }

            if (code === 45 || code === 48) {
                // Fog: no sky glyph, just bands.
                drawFog(lit, dim)
            } else if (code === 0 || (code >= 1 && code <= 2)) {
                // Clear/partly cloudy: the sun or moon shrinks into the
                // top-right corner once a cloud joins it.
                const partly = code >= 1 && code <= 2
                const skyX = partly ? w * 0.62 : cx
                const skyY = partly ? h * 0.38 : h * 0.42
                const skyR = partly ? s * 0.22 : s * 0.28
                if (icon.day)
                    drawSun(skyX, skyY, skyR, lit)
                else
                    drawMoon(skyX, skyY, skyR, lit)
                if (partly)
                    drawCloud(w * 0.42, h * 0.66, s * 0.4, dim)
            } else if (code === 3) {
                // Overcast: two cloud layers, no sky glyph.
                drawCloud(w * 0.38, h * 0.42, s * 0.32, dim)
                drawCloud(w * 0.56, h * 0.6, s * 0.4, lit)
            } else {
                // Everything else is a cloud plus precipitation or a bolt,
                // with the showers groups getting a sun/moon peeking from
                // the top-left to set them apart from steady rain/snow.
                const showers = (code >= 80 && code <= 82) || code === 85 || code === 86
                if (showers) {
                    if (icon.day)
                        drawSun(w * 0.28, h * 0.3, s * 0.16, lit)
                    else
                        drawMoon(w * 0.28, h * 0.3, s * 0.16, lit)
                }
                drawCloud(w * 0.54, h * 0.44, s * 0.4, lit)
                if (code >= 51 && code <= 57)
                    drawDrops(w * 0.54, h * 0.72, s * 0.4, dim, 3, s * 0.18)
                else if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82))
                    drawDrops(w * 0.54, h * 0.72, s * 0.4, lit, 3, s * 0.3)
                else if ((code >= 71 && code <= 77) || code === 85 || code === 86)
                    drawFlakes(w * 0.54, h * 0.74, s * 0.4, lit, 3)
                else if (code >= 95 && code <= 99)
                    drawBolt(w * 0.54, h * 0.68, s * 0.34, lit)
            }
        }
    }
}
