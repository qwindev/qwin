import QtQuick
import qwin 1.0
import "../shared"

// Caffeine: a click-to-arm keep-awake toggle over Power.keepAwake. A filled
// cup with steam means armed, a hollow outline idle - a shape difference,
// not just a colour shift, so it reads at bar size. No popup; a second click
// disarms.
//
// config.json section (all keys optional):
//   "caffeine": {
//       "minutes": 0   // auto-disarm after N minutes (0 = indefinite)
//   }
Rectangle {
    id: caffeineItem

    // Read once per load; a config.json edit reloads the plugin anyway.
    readonly property var cfg: Plugins.config("caffeine")
    readonly property int minutesCfg: cfg.minutes || 0

    property int remainingSeconds: 0

    // "M:SS" under an hour, "Hh" above, padded to a constant 5 characters so
    // the ticking countdown does not reflow the bar.
    function remainingText() {
        const total = Math.max(0, Math.ceil(remainingSeconds))
        if (total >= 3600)
            return (Math.ceil(total / 3600) + "h").padStart(5)
        const m = Math.floor(total / 60)
        const s = total % 60
        return (m + ":" + String(s).padStart(2, "0")).padStart(5)
    }

    width: row.implicitWidth + 14
    height: 24
    radius: 5
    // An accent tint under the hover highlight, so "on" reads before the
    // glyph is examined.
    color: Power.keepAwake ? Qt.alpha(Colors.accent, mouse.containsMouse ? 0.26 : 0.15)
                            : (mouse.containsMouse ? Qt.alpha(Colors.surface, 0.13) : "transparent")

    // Auto-disarm countdown. `running` binds to Power.keepAwake itself, not
    // a private "I armed this" flag, so the countdown stops however keepAwake
    // goes false; onRunningChanged reseeds it whenever arming starts it.
    Timer {
        id: countdown
        interval: 1000
        repeat: true
        running: Power.keepAwake && caffeineItem.minutesCfg > 0
        onRunningChanged: if (running) caffeineItem.remainingSeconds = caffeineItem.minutesCfg * 60
        onTriggered: {
            caffeineItem.remainingSeconds -= 1
            if (caffeineItem.remainingSeconds <= 0)
                Power.keepAwake = false // `running` above follows on its own
        }
    }

    // Coffee-cup glyph on a Canvas: filled with steam while armed, a hollow
    // outline while idle. The steam is a static shape painted per
    // requestPaint(), never an animation - a bar glyph must not keep moving.
    component CaffeineGlyph: Item {
        id: glyph
        width: 18
        height: 20

        property bool active: false
        property color litColor: "white"
        property color idleColor: "gray"

        onActiveChanged: canvas.requestPaint()
        onLitColorChanged: canvas.requestPaint()
        onIdleColorChanged: canvas.requestPaint()

        Canvas {
            id: canvas
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const w = width
                const h = height
                const color = glyph.active ? glyph.litColor : glyph.idleColor

                const cupTop = h * 0.50
                const cupBottom = h * 0.90
                const cupLeft = w * 0.10
                const cupRight = w * 0.62
                const cupW = cupRight - cupLeft
                const r = 2.2

                function cupPath() {
                    ctx.beginPath()
                    ctx.moveTo(cupLeft, cupTop)
                    ctx.lineTo(cupRight, cupTop)
                    ctx.lineTo(cupRight, cupBottom - r)
                    ctx.quadraticCurveTo(cupRight, cupBottom, cupRight - r, cupBottom)
                    ctx.lineTo(cupLeft + r, cupBottom)
                    ctx.quadraticCurveTo(cupLeft, cupBottom, cupLeft, cupBottom - r)
                    ctx.closePath()
                }

                ctx.strokeStyle = color
                ctx.fillStyle = color
                ctx.lineWidth = 1.3

                cupPath()
                // Filled vs hollow: a colour shift alone is too subtle at 24px.
                if (glyph.active)
                    ctx.fill()
                else
                    ctx.stroke()

                // Handle, off the cup's right edge.
                ctx.beginPath()
                ctx.arc(cupRight + w * 0.10, (cupTop + cupBottom) / 2, h * 0.16,
                        -Math.PI * 0.55, Math.PI * 0.55)
                ctx.stroke()

                // Steam, armed only.
                if (glyph.active) {
                    const steamY0 = cupTop - 1.5
                    const xs = [cupLeft + cupW * 0.22, cupLeft + cupW * 0.5, cupLeft + cupW * 0.78]
                    ctx.lineWidth = 1.1
                    for (let i = 0; i < xs.length; i++) {
                        const x = xs[i]
                        ctx.beginPath()
                        ctx.moveTo(x, steamY0)
                        ctx.bezierCurveTo(x - 2.2, steamY0 - 3, x + 2.2, steamY0 - 6, x, steamY0 - 9)
                        ctx.stroke()
                    }
                }
            }
        }
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6

        CaffeineGlyph {
            anchors.verticalCenter: parent.verticalCenter
            active: Power.keepAwake
            litColor: Colors.accent
            idleColor: Colors.textMuted
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: countdown.running
            text: caffeineItem.remainingText()
            color: Colors.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: Power.keepAwake = !Power.keepAwake
    }
}
