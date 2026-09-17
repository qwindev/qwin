import QtQuick
import QtQuick.Window
import Qwin
import Qwin.Ui

// Workspace switcher: one button per workspace on this module's monitor.
// Workspaces belong to the `Tiler` singleton, not the OS - switching one
// cloaks and uncloaks windows - and the count is fixed by config rather than
// created or closed from here. Buttons only - the switch and move chords
// live in `plugins/tiling` under `tiling.keys`. Meant for the bar, but works
// standalone - an Item root gets the default wrapper.
//
// Inside a PanelWindow (the bundled `bar`), `device` reads the monitor that
// panel is docked to off `Window.window.device` and this module shows and
// drives THAT monitor's workspaces - not necessarily the tiler's focused
// one. Standalone (device empty) it falls back to today's behaviour: the
// tiler's focused monitor. Never Screen.name - see screendevice.h's doc
// comment for why a QScreen name cannot key a monitor.
//
// config.json section (all keys optional):
//   "workspaces": {
//       "showEmpty": true    // false draws only the workspaces that have
//                            // windows, plus the active one
//   }
Row {
    id: workspaces
    spacing: 5

    readonly property var cfg: Plugins.config("workspaces")
    readonly property bool showEmpty: cfg.showEmpty !== false

    readonly property string device: (Window.window && Window.window.device) || ""
    // Live per-monitor entry from Tiler.monitors when `device` names one;
    // undefined standalone or before the tiler has ever seen that monitor.
    readonly property var mon: Tiler.monitors[device]

    Repeater {
        model: workspaces.mon ? workspaces.mon.workspaces : Tiler.workspaces

        Rectangle {
            id: wsButton
            required property var modelData
            readonly property int index: modelData.index
            readonly property bool active: modelData.active
            readonly property int windowCount: modelData.windows
            // This monitor's active button keeps the accent look only when
            // it is also the one the next chord would land on - standalone
            // (no `mon`) it always is, since it is reading the focused
            // monitor's own list.
            readonly property bool focusedMonitor: !workspaces.mon || workspaces.mon.focused

            // The active workspace always shows, even while empty: with
            // showEmpty off the row would otherwise omit where you are.
            visible: wsButton.windowCount > 0 || wsButton.active || workspaces.showEmpty
            width: 24
            height: 24
            radius: 5
            opacity: wsButton.active || wsButton.windowCount > 0 ? 1 : 0.4
            color: wsButton.active && wsButton.focusedMonitor ? Qt.alpha(Colors.accent, 0.2)
                                   : (wsMouse.containsMouse ? Qt.alpha(Colors.surface, 0.13) : "transparent")
            border.color: wsButton.active ? (wsButton.focusedMonitor ? Colors.accent : Colors.textMuted)
                                          : Qt.alpha(Colors.surface, 0.2)
            border.width: 1

            Text {
                anchors.centerIn: parent
                text: wsButton.index + 1
                color: wsButton.active ? (wsButton.focusedMonitor ? Colors.accent : Colors.textMuted)
                                       : Colors.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.bold: wsButton.active
            }

            MouseArea {
                id: wsMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: workspaces.device ? Tiler.switchToWorkspaceOn(workspaces.device, wsButton.index)
                                             : Tiler.switchToWorkspace(wsButton.index)
            }
        }
    }
}
