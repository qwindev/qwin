import QtQuick
import QtQml.Models
import Qwin
import "../shared"

// Workspace switcher: one button per workspace on the tiler's focused
// monitor. Workspaces belong to the `Tiler` singleton, not the OS - switching
// one cloaks and uncloaks windows - and the count is fixed by config rather
// than created or closed from here. Meant for the bar, but works standalone -
// an Item root gets the default wrapper.
//
// config.json section (all keys optional):
//   "workspaces": {
//       "switchKey": "Shift+Alt+{n}",  // {n} -> 1..min(Tiler.workspaceCount, 9)
//       "moveKey": "Ctrl+Alt+{n}",     // moves the foreground window, does not follow it
//       "moveToEmptyKey": "Shift+Alt+M", // window -> first empty workspace, and follow
//       "showEmpty": true              // false draws only the workspaces that
//                                      // have windows, plus the active one
//   }
Row {
    id: workspaces
    spacing: 5

    readonly property var cfg: Plugins.config("workspaces")
    readonly property string switchTemplate: cfg.switchKey !== undefined ? cfg.switchKey : "Shift+Alt+{n}"
    readonly property string moveTemplate: cfg.moveKey !== undefined ? cfg.moveKey : "Ctrl+Alt+{n}"
    readonly property string moveToEmptyKey: cfg.moveToEmptyKey !== undefined ? cfg.moveToEmptyKey : "Shift+Alt+M"
    readonly property bool showEmpty: cfg.showEmpty !== false

    // Shift+Alt+1..9 from any application, mirroring the buttons below. At
    // row level, not inside them: a hotkey belongs to the workspace set, not
    // to one button. Capped at 9 - there is no single key left beyond that.
    Instantiator {
        model: Math.min(Tiler.workspaceCount, 9)

        Hotkey {
            required property int index
            // index goes -1 while the Instantiator tears an item down; an
            // empty sequence stops it re-registering on a stale number.
            sequence: index >= 0 ? workspaces.switchTemplate.replace("{n}", index + 1) : ""
            onActivated: Tiler.switchToWorkspace(index)
        }
    }

    // Ctrl+Alt+1..9: send the foreground window to that workspace without
    // following it. Must be a hotkey, not a button: a chord does not change
    // focus, so the foreground window is still the one the user was in.
    Instantiator {
        model: Math.min(Tiler.workspaceCount, 9)

        Hotkey {
            required property int index
            sequence: index >= 0 ? workspaces.moveTemplate.replace("{n}", index + 1) : ""
            onActivated: Tiler.moveToWorkspace(index)
        }
    }

    // Send the focused window to the first empty workspace and go with it -
    // Hyprland's `movetoworkspace, empty`. A hotkey rather than a button for
    // the same reason as the move chords above.
    Hotkey {
        sequence: workspaces.moveToEmptyKey
        onActivated: Tiler.moveToEmptyWorkspace()
    }

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
