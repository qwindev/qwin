import QtQuick
import Qwin
import Qwin.Ui

// WiFi indicator: strength glyph and signal percentage in the bar, with the
// SSID, live rates and switch button in the popup. Hides itself with no WLAN
// adapter - `shown` for the bar's Loader, `visible` for standalone use.
Rectangle {
    id: wifiItem
    property bool shown: Wifi.available
    visible: shown

    // With Location services off the SSID and signal are unavailable but
    // connectivity is still known, so light the glyph fully rather than dead.
    readonly property int level: Wifi.detailsAvailable ? Wifi.signalPercent
                                                       : (Wifi.connected ? 100 : 0)

    // A constant 6 characters ("   12K", "999.9M"), so the down and up
    // figures line up and a digit change does not reflow the popup.
    function speedText(bytesPerSec) {
        const kb = bytesPerSec / 1024
        if (kb < 1)
            return "0K".padStart(6)
        if (kb < 1000)
            return (Math.round(kb) + "K").padStart(6)
        const mb = kb / 1024
        if (mb < 1000)
            return (mb.toFixed(1) + "M").padStart(6)
        return ((mb / 1024).toFixed(1) + "G").padStart(6)
    }

    width: wifiRow.implicitWidth + 14
    height: 24
    radius: 5
    color: wifiMouse.containsMouse || wifiMenu.opened ? Qt.alpha(Colors.surface, 0.13)
                                                      : "transparent"

    // WiFi strength glyph: a dot and three arcs on a Canvas, so no icon font
    // is needed. Arcs light with signal strength; `off` dims everything and
    // draws a slash.
    component WifiGlyph: Item {
        id: glyph

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

                ctx.fillStyle = (!glyph.off && glyph.percent > 0) ? glyph.litColor : glyph.dimColor
                ctx.beginPath()
                ctx.arc(cx, cy, ctx.lineWidth * 0.8, 0, Math.PI * 2)
                ctx.fill()

                for (let i = 0; i < 3; i++) {
                    const lit = !glyph.off && glyph.percent >= thresholds[i]
                    ctx.strokeStyle = lit ? glyph.litColor : glyph.dimColor
                    ctx.beginPath()
                    ctx.arc(cx, cy, (i + 1) * h * 0.27, -Math.PI * 0.75, -Math.PI * 0.25)
                    ctx.stroke()
                }

                if (glyph.off) {
                    ctx.strokeStyle = glyph.dimColor
                    ctx.beginPath()
                    ctx.moveTo(w * 0.15, h * 0.08)
                    ctx.lineTo(w * 0.85, h * 0.92)
                    ctx.stroke()
                }
            }
        }
    }

    // The popup action button - a bordered, hoverable row for the popup's
    // single hand-off action ("Switch network").
    component PopupButton: Rectangle {
        id: button

        property string label

        signal clicked()

        height: 32
        radius: 8
        color: mouse.containsMouse ? Qt.alpha(Colors.accent, 0.25)
                                    : Qt.alpha(Colors.surface, 0.13)
        border.color: Qt.alpha(Colors.accent, 0.4)
        border.width: 1

        Text {
            anchors.centerIn: parent
            text: button.label
            color: Colors.text
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.clicked()
        }
    }

    Row {
        id: wifiRow
        anchors.centerIn: parent
        spacing: 6

        WifiGlyph {
            anchors.verticalCenter: parent.verticalCenter
            width: 15; height: 15
            percent: wifiItem.level
            off: !Wifi.connected
            litColor: Colors.text
            dimColor: Qt.alpha(Colors.textMuted, 0.4)
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: Wifi.connected && Wifi.detailsAvailable
            text: Wifi.signalPercent + "%"
            color: Colors.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }
    }

    MouseArea {
        id: wifiMouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: wifiMenu.toggle()
    }

    Popup {
        id: wifiMenu
        anchorItem: wifiItem
        contentWidth: 260
        contentHeight: wifiMenuColumn.implicitHeight + 24
        backgroundColor: Colors.background
        borderColor: Qt.alpha(Colors.accent, 0.2)

        Column {
            id: wifiMenuColumn
            width: parent.width
            spacing: 12

            Row {
                spacing: 8

                WifiGlyph {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 16; height: 16
                    percent: wifiItem.level
                    off: !Wifi.connected
                    litColor: Wifi.connected ? Colors.accent : Colors.textMuted
                    dimColor: Qt.alpha(Colors.textMuted, 0.4)
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: !Wifi.connected ? "Not connected"
                                          : (Wifi.detailsAvailable ? Wifi.ssid : "Connected")
                    color: Wifi.connected ? Colors.text : Colors.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.bold: Wifi.connected
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, 160)
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: Wifi.connected && Wifi.detailsAvailable
                    text: Wifi.signalPercent + "%"
                    color: Colors.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                }
            }

            // Room for the units the bar has to leave off.
            Text {
                visible: Wifi.connected
                text: "\u2193" + wifiItem.speedText(Wifi.rxBytesPerSec) + "B/s"
                      + "   \u2191" + wifiItem.speedText(Wifi.txBytesPerSec) + "B/s"
                color: Colors.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }

            // Say why the name is missing, rather than show a nameless
            // connection.
            Text {
                visible: Wifi.connected && !Wifi.detailsAvailable
                width: parent.width
                text: "Turn on Location services to see the network name and signal."
                color: Colors.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            // Switching happens in the native flyout.
            PopupButton {
                width: parent.width
                label: "Switch network"
                onClicked: {
                    Wifi.openNetworkFlyout()
                    wifiMenu.dismiss()
                }
            }
        }
    }
}
