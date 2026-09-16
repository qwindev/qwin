import QtQuick
import Qwin
import Qwin.Ui

// Workspace switcher: one button per workspace on the tiler's focused
// monitor. Workspaces belong to the `Tiler` singleton, not the OS - switching
// one cloaks and uncloaks windows - and the count is fixed by config rather
// than created or closed from here. Buttons only - the switch and move
// chords live in `plugins/tiling` under `tiling.keys`. Meant for the bar,
// but works standalone - an Item root gets the default wrapper.
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

    Repeater {
        model: Tiler.workspaces

        Rectangle {
            id: wsButton
            required property var modelData
            readonly property int index: modelData.index
            readonly property bool active: modelData.active
            readonly property int windowCount: modelData.windows

            // The active workspace always shows, even while empty: with
            // showEmpty off the row would otherwise omit where you are.
            visible: wsButton.windowCount > 0 || wsButton.active || workspaces.showEmpty
            width: 24
            height: 24
            radius: 5
            opacity: wsButton.active || wsButton.windowCount > 0 ? 1 : 0.4
            color: wsButton.active ? Qt.alpha(Colors.accent, 0.2)
                                   : (wsMouse.containsMouse ? Qt.alpha(Colors.surface, 0.13) : "transparent")
            border.color: wsButton.active ? Colors.accent : Qt.alpha(Colors.surface, 0.2)
            border.width: 1

            Text {
                anchors.centerIn: parent
                text: wsButton.index + 1
                color: wsButton.active ? Colors.accent : Colors.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.bold: wsButton.active
            }

            MouseArea {
                id: wsMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: Tiler.switchToWorkspace(wsButton.index)
            }
        }
    }
}
