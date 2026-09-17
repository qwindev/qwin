import QtQuick
import QtQuick.Window
import Qwin
import Qwin.Ui

// Bar module: whether the tiler is on and how many tiles are on screen; a
// click toggles it (global - `Tiler.enabled` is a single master switch, not
// per-monitor). Config and chords live in `plugins/tiling` - without that
// plugin a click still tiles, but on built-in defaults and with no chords.
//
// Inside a PanelWindow, `device` reads the monitor that panel is docked to
// off `Window.window.device` and the tile count is THAT monitor's, not the
// whole desktop's; standalone (device empty) it falls back to
// Tiler.managedCount, as before this property existed.
Rectangle {
    id: indicator
    readonly property string device: (Window.window && Window.window.device) || ""
    readonly property var mon: Tiler.monitors[device]
    readonly property int tileCount: mon ? mon.tiles : Tiler.managedCount

    width: row.implicitWidth + 14
    height: 24
    radius: 5
    color: Tiler.enabled ? Qt.alpha(Colors.accent, mouse.containsMouse ? 0.26 : 0.15)
                         : (mouse.containsMouse ? Qt.alpha(Colors.surface, 0.13) : "transparent")

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6

        // The dwindle partition itself as the glyph: one vertical divider,
        // then a horizontal one in the smaller half. Filled panes while
        // tiling is on, outline while off - a shape difference, not just a
        // colour shift, so it reads at bar size.
        Canvas {
            id: glyph
            width: 16
            height: 14
            anchors.verticalCenter: parent.verticalCenter

            readonly property bool active: Tiler.enabled
            readonly property color tint: active ? Colors.accent : Colors.textMuted

            onActiveChanged: requestPaint()
            onTintChanged: requestPaint()

            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = glyph.tint
                ctx.fillStyle = glyph.tint
                ctx.lineWidth = 1.2

                const split = Math.round(width * 0.55)
                const half = Math.round(height * 0.5)
                const panes = [
                    Qt.rect(0.6, 0.6, split - 1.8, height - 1.2),
                    Qt.rect(split + 0.6, 0.6, width - split - 1.2, half - 1.2),
                    Qt.rect(split + 0.6, half + 0.6, width - split - 1.2, height - half - 1.2)
                ]
                for (let i = 0; i < panes.length; i++) {
                    const p = panes[i]
                    ctx.beginPath()
                    ctx.rect(p.x, p.y, p.width, p.height)
                    if (glyph.active)
                        ctx.fill()
                    else
                        ctx.stroke()
                }
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: indicator.tileCount
            color: Tiler.enabled ? Colors.accent : Colors.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: Tiler.enabled = !Tiler.enabled
    }
}
