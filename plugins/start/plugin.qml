import QtQuick
import qwin 1.0

// Start button: the four-pane Windows logo, clicked to open the Start menu
// through System.openStartMenu() (which toggles, so a second click closes
// it). No popup of our own - the menu is the shell's, and the bar is
// WS_EX_NOACTIVATE, so the click leaves the foreground alone for it to take.
//
// config.json section (all keys optional):
//   "start": {
//       "color": "#0078D4",   // fixed logo colour; "" follows the palette
//       "size": 14            // logo edge in pixels
//   }
Rectangle {
    id: startItem

    // Read once per load; a config.json edit reloads the plugin anyway.
    readonly property var cfg: Plugins.config("start")
    readonly property string colorCfg: cfg.color || ""
    readonly property int logoSize: cfg.size || 14

    // Gap as a share of the size, so the four panes stay separated at any
    // `size`; the pane takes what is left, floored to whole pixels because a
    // fractional square renders a blurred edge.
    readonly property int gap: Math.max(1, Math.round(logoSize * 0.14))
    readonly property int pane: Math.max(1, Math.floor((logoSize - gap) / 2))

    // A fixed colour stays put under the hover highlight - the Windows blue
    // brightening to the accent would read as a different logo.
    readonly property color logoColor:
        colorCfg !== "" ? colorCfg
                        : (mouse.containsMouse ? Colors.accent : Colors.textMuted)

    width: logoSize + 14
    height: 24
    radius: 5
    color: mouse.containsMouse ? Qt.alpha(Colors.surface, 0.13) : "transparent"

    // Windows 11's logo: four flat squares, without the perspective skew the
    // 8/10 one had.
    Grid {
        anchors.centerIn: parent
        columns: 2
        rows: 2
        spacing: startItem.gap

        Repeater {
            model: 4

            Rectangle {
                width: startItem.pane
                height: startItem.pane
                color: startItem.logoColor
            }
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: System.openStartMenu()
    }
}
