import QtQuick
import qwin 1.0
import "../shared"

// Bluetooth indicator: rune glyph and connected count in the bar, with the
// paired-device list and settings shortcut in the popup. Minimal like Wifi -
// pairing, connect/disconnect and scanning all go through Windows' own
// settings page. Hides itself with no radio: `shown` for the bar's Loader,
// `visible` for standalone use.
//
// config.json section (all keys optional):
//   "bluetooth": {
//       "showCount": true,    // the connected count next to the glyph
//       "maxNameWidth": 170   // px a device name elides to in the popup
//   }
Rectangle {
    id: bluetoothItem
    property bool shown: Bluetooth.available
    visible: shown

    // Read once per load; a config.json edit triggers a full reload anyway.
    readonly property var cfg: Plugins.config("bluetooth")
    readonly property bool showCount: cfg.showCount !== false
    readonly property int maxNameWidth: cfg.maxNameWidth || 170

    // Bluetooth.devices comes in Windows' enumeration order; the popup wants
    // connected first. A NEW array, not a sort in place, so the property read
    // inside re-evaluates this whenever the singleton changes.
    function sortedDevices() {
        const list = Bluetooth.devices.slice()
        list.sort((a, b) => (a.connected === b.connected) ? 0 : (a.connected ? -1 : 1))
        return list
    }

    width: bluetoothRow.implicitWidth + 14
    height: 24
    radius: 5
    color: bluetoothMouse.containsMouse || bluetoothMenu.opened ? Qt.alpha(Colors.surface, 0.13)
                                                                 : "transparent"

    // The Bluetooth bind-rune, traced as one open stroked path: a top and a
    // bottom point sharing the center x, joined by two long diagonals that
    // cross in the middle to form the flags. Inline like BatteryGlyph, since
    // nothing else needs it.
    component BluetoothGlyph: Item {
        id: glyph
        width: 14
        height: 16

        property color glyphColor: "white"

        onGlyphColorChanged: canvas.requestPaint()
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

                const midX = w * 0.5
                const leftX = w * 0.2
                const rightX = w * 0.8
                const top = h * 0.06
                const upper = h * 0.28
                const bottom = h * 0.94
                const lower = h * 0.72

                ctx.strokeStyle = glyph.glyphColor
                ctx.lineWidth = Math.max(1.2, w * 0.12)
                ctx.lineJoin = "round"
                ctx.lineCap = "round"
                ctx.beginPath()
                ctx.moveTo(midX, top)
                ctx.lineTo(rightX, upper)
                ctx.lineTo(leftX, lower)
                ctx.lineTo(midX, bottom)
                ctx.lineTo(rightX, lower)
                ctx.lineTo(leftX, upper)
                ctx.closePath()
                ctx.stroke()
            }
        }
    }

    Row {
        id: bluetoothRow
        anchors.centerIn: parent
        spacing: 6

        BluetoothGlyph {
            anchors.verticalCenter: parent.verticalCenter
            width: 14; height: 16
            glyphColor: Bluetooth.connectedCount > 0 ? Colors.text : Colors.textMuted
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            // A lone dimmed glyph reads as "on, nothing connected", so the
            // count only earns a slot above 0. Positioners skip invisible
            // children, so hiding this leaves no gap.
            visible: bluetoothItem.showCount && Bluetooth.connectedCount > 0
            // A constant 2 characters, so the bar does not reflow at a digit
            // boundary.
            text: String(Bluetooth.connectedCount).padStart(2)
            color: Colors.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }
    }

    MouseArea {
        id: bluetoothMouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: bluetoothMenu.toggle()
    }

    Popup {
        id: bluetoothMenu
        anchorItem: bluetoothItem
        contentWidth: bluetoothItem.maxNameWidth + 90
        contentHeight: bluetoothMenuColumn.implicitHeight + 24
        backgroundColor: Colors.background
        borderColor: Qt.alpha(Colors.accent, 0.2)

        // The singleton polls every 5 s; refresh so the list is fresh when
        // the user actually looks at it.
        onOpenedChanged: if (opened) Bluetooth.refresh()

        Column {
            id: bluetoothMenuColumn
            width: parent.width
            spacing: 10

            Text {
                visible: deviceRepeater.count === 0
                text: "No paired devices"
                color: Colors.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 13
            }

            Repeater {
                id: deviceRepeater
                model: bluetoothItem.sortedDevices()

                Row {
                    required property var modelData
                    width: bluetoothMenuColumn.width
                    spacing: 8

                    // Filled when connected, hollow otherwise.
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 8; height: 8
                        radius: 4
                        color: modelData.connected ? Colors.accent : "transparent"
                        border.color: modelData.connected ? Colors.accent : Colors.textMuted
                        border.width: 1
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.name
                        color: modelData.connected ? Colors.text : Colors.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        elide: Text.ElideRight
                        width: Math.min(implicitWidth, bluetoothItem.maxNameWidth)
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: modelData.battery >= 0
                        text: modelData.battery + "%"
                        color: Colors.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                    }
                }
            }

            // Pairing, connecting and the radio toggle live in Windows' own
            // settings page.
            PopupButton {
                width: parent.width
                label: "Bluetooth settings"
                accentColor: Colors.accent
                surfaceColor: Colors.surface
                textColor: Colors.text
                onClicked: {
                    Qt.openUrlExternally("ms-settings:bluetooth")
                    bluetoothMenu.dismiss()
                }
            }
        }
    }
}
