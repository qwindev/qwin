import QtQuick
import Qwin
import "../shared"

// Battery indicator: charge glyph and percentage in the bar, with charge
// state, time remaining and the settings shortcut in the popup. Hides itself
// on a desktop - `shown` for the bar's Loader, `visible` for standalone use.
Rectangle {
    id: batteryItem
    property bool shown: System.batteryAvailable
    visible: shown

    // Charging always reads accent: the cable being in matters more than the
    // level. On battery the fill warns near empty, readable from the bar.
    readonly property color glyphColor: System.batteryCharging ? Colors.accent
                                       : (System.batteryPercent >= 0 && System.batteryPercent <= 10) ? Colors.error
                                       : (System.batteryPercent >= 0 && System.batteryPercent <= 20) ? Colors.warning
                                       : Colors.text

    // A constant 4 characters ("  --", " 5%", "100%"), so the bar does not
    // reflow at a digit boundary.
    function percentText() {
        return (System.batteryPercent < 0 ? "--" : (System.batteryPercent + "%")).padStart(4)
    }

    function timeText(seconds) {
        const mins = Math.floor(seconds / 60)
        const h = Math.floor(mins / 60)
        const m = mins % 60
        return (h > 0 ? (h + " h ") : "") + m + " min remaining"
    }

    width: batteryRow.implicitWidth + 14
    height: 24
    radius: 5
    color: batteryMouse.containsMouse || batteryMenu.opened ? Qt.alpha(Colors.surface, 0.13)
                                                             : "transparent"

    // Low-battery pulse, only below 10% and unplugged. The percent >= 0
    // guard stops an unknown reading (-1) satisfying "<= 10";
    // onRunningChanged resets opacity so a stop mid-fade leaves nothing dim.
    SequentialAnimation {
        id: pulseAnim
        loops: Animation.Infinite
        running: System.batteryPercent >= 0 && System.batteryPercent <= 10 && !System.acPower
        onRunningChanged: if (!running) batteryItem.opacity = 1
        NumberAnimation { target: batteryItem; property: "opacity"; to: 0.4; duration: 700; easing.type: Easing.InOutQuad }
        NumberAnimation { target: batteryItem; property: "opacity"; to: 1.0; duration: 700; easing.type: Easing.InOutQuad }
    }

    // Battery glyph on a Canvas: body, cap, a fill proportional to charge,
    // and a bolt while charging. Inline rather than in shared/, since nothing
    // else needs it; the requestPaint() shape mirrors shared/WifiIcon.qml.
    component BatteryGlyph: Item {
        id: glyph
        width: 20
        height: 11

        property int percent: 0
        property bool charging: false
        property color fillColor: "white"
        property color outlineColor: "#66888888"
        property color boltColor: "black"

        onPercentChanged: canvas.requestPaint()
        onChargingChanged: canvas.requestPaint()
        onFillColorChanged: canvas.requestPaint()
        onOutlineColorChanged: canvas.requestPaint()
        onBoltColorChanged: canvas.requestPaint()

        Canvas {
            id: canvas
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const w = width
                const h = height
                const capW = Math.max(1.5, w * 0.10)
                const bodyW = w - capW - 1
                const r = h * 0.25

                function roundedRect(x, y, rw, rh, rr) {
                    ctx.beginPath()
                    ctx.moveTo(x + rr, y)
                    ctx.lineTo(x + rw - rr, y)
                    ctx.arcTo(x + rw, y, x + rw, y + rr, rr)
                    ctx.lineTo(x + rw, y + rh - rr)
                    ctx.arcTo(x + rw, y + rh, x + rw - rr, y + rh, rr)
                    ctx.lineTo(x + rr, y + rh)
                    ctx.arcTo(x, y + rh, x, y + rh - rr, rr)
                    ctx.lineTo(x, y + rr)
                    ctx.arcTo(x, y, x + rr, y, rr)
                    ctx.closePath()
                }

                // Body.
                ctx.strokeStyle = glyph.outlineColor
                ctx.lineWidth = 1.2
                roundedRect(0.6, 0.6, bodyW - 1.2, h - 1.2, r)
                ctx.stroke()

                // Cap.
                const capH = h * 0.42
                ctx.fillStyle = glyph.outlineColor
                ctx.fillRect(bodyW, (h - capH) / 2, capW, capH)

                // Fill, proportional to charge.
                const pct = Math.max(0, Math.min(100, glyph.percent)) / 100
                const pad = 2
                const fillMaxW = bodyW - pad * 2
                const fillW = fillMaxW * pct
                if (fillW > 0.5) {
                    ctx.fillStyle = glyph.fillColor
                    roundedRect(pad, pad, fillW, h - pad * 2, Math.min(r * 0.6, fillW / 2))
                    ctx.fill()
                }

                // The bolt sits mid-body, so one pass in boltColor vanishes
                // below roughly two thirds of charge, where that middle is
                // still background. Two passes under opposite clips -
                // boltColor inside the fill, fillColor outside - keep it
                // legible at every level.
                if (glyph.charging) {
                    function boltPath() {
                        ctx.beginPath()
                        ctx.moveTo(bodyW * 0.56, pad)
                        ctx.lineTo(bodyW * 0.34, h * 0.58)
                        ctx.lineTo(bodyW * 0.48, h * 0.58)
                        ctx.lineTo(bodyW * 0.40, h - pad)
                        ctx.lineTo(bodyW * 0.66, h * 0.40)
                        ctx.lineTo(bodyW * 0.50, h * 0.40)
                        ctx.closePath()
                    }

                    ctx.save()
                    ctx.beginPath()
                    ctx.rect(pad, 0, Math.max(fillW, 0), h)
                    ctx.clip()
                    ctx.fillStyle = glyph.boltColor
                    boltPath()
                    ctx.fill()
                    ctx.restore()

                    ctx.save()
                    ctx.beginPath()
                    ctx.rect(pad + Math.max(fillW, 0), 0, bodyW, h)
                    ctx.clip()
                    ctx.fillStyle = glyph.fillColor
                    boltPath()
                    ctx.fill()
                    ctx.restore()
                }
            }
        }
    }

    Row {
        id: batteryRow
        anchors.centerIn: parent
        spacing: 6

        BatteryGlyph {
            anchors.verticalCenter: parent.verticalCenter
            percent: System.batteryPercent < 0 ? 0 : System.batteryPercent
            charging: System.batteryCharging
            fillColor: batteryItem.glyphColor
            outlineColor: Qt.alpha(Colors.textMuted, 0.6)
            boltColor: Colors.background
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: batteryItem.percentText()
            color: Colors.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }
    }

    MouseArea {
        id: batteryMouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: batteryMenu.toggle()
    }

    Popup {
        id: batteryMenu
        anchorItem: batteryItem
        contentWidth: 230
        contentHeight: batteryMenuColumn.implicitHeight + 24
        backgroundColor: Colors.background
        borderColor: Qt.alpha(Colors.accent, 0.2)

        Column {
            id: batteryMenuColumn
            width: parent.width
            spacing: 12

            Text {
                text: System.batteryPercent < 0 ? "--" : (System.batteryPercent + "%")
                color: Colors.text
                font.family: Theme.fontFamily
                font.pixelSize: 22
                font.bold: true
            }

            Text {
                text: System.batteryCharging ? "Charging"
                    : (System.acPower ? "Plugged in, fully charged" : "On battery")
                color: Colors.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 13
            }

            Text {
                visible: System.batteryTimeLeft >= 0
                text: batteryItem.timeText(System.batteryTimeLeft)
                color: Colors.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }

            Text {
                visible: System.batterySaver
                text: "Battery saver on"
                color: Colors.warning
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }

            // Windows owns power-plan and sleep settings; deep-link rather
            // than duplicate the picker.
            PopupButton {
                width: parent.width
                label: "Power settings"
                accentColor: Colors.accent
                surfaceColor: Colors.surface
                textColor: Colors.text
                onClicked: {
                    Qt.openUrlExternally("ms-settings:powersleep")
                    batteryMenu.dismiss()
                }
            }
        }
    }
}
