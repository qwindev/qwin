import QtQuick
import QtQml.Models
import qwin 1.0
import "../shared"

// Workspace switcher: one button per virtual desktop plus a "+". Meant for
// the bar, but works standalone - an Item root gets the default wrapper.
Row {
    id: workspaces
    spacing: 5

    // Shift+Alt+1..9 from any application, mirroring the buttons below. At
    // row level, not inside them: a hotkey belongs to a desktop, not a button.
    Instantiator {
        model: Math.min(Desktops.count, 9)

        Hotkey {
            required property int index
            // index goes -1 while the Instantiator tears an item down; an
            // empty sequence stops it re-registering as "Shift+Alt+0".
            sequence: index >= 0 ? "Shift+Alt+" + (index + 1) : ""
            onActivated: Desktops.switchTo(index)
        }
    }

    // Mirrors the "+" button below.
    Hotkey {
        sequence: "Shift+Alt+A"
        onActivated: Desktops.createDesktop()
    }

    // Close the active desktop; a warning no-op on the last one.
    Hotkey {
        sequence: "Shift+Alt+X"
        onActivated: Desktops.closeCurrentDesktop()
    }

    // Throw the focused window onto a fresh desktop and follow it. Must be a
    // hotkey, not a button: a chord does not change focus, so the foreground
    // window is still the one the user was working in.
    Hotkey {
        sequence: "Shift+Alt+M"
        onActivated: Desktops.moveForegroundWindowToNewDesktop()
    }

    Repeater {
        model: Desktops.count

        Rectangle {
            id: wsButton
            required property int index
            readonly property bool active: index === Desktops.currentIndex

            width: 24
            height: 24
            radius: 5
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
                onClicked: Desktops.switchTo(wsButton.index)
            }
        }
    }

    // Same as Shift+Alt+A above.
    Rectangle {
        id: addButton
        visible: Desktops.available
        width: 24
        height: 24
        radius: 5
        color: addMouse.containsMouse ? Qt.alpha(Colors.surface, 0.13) : "transparent"
        border.color: Qt.alpha(Colors.surface, 0.2)
        border.width: 1

        Text {
            anchors.centerIn: parent
            text: "+"
            color: Colors.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 14
        }

        MouseArea {
            id: addMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: Desktops.createDesktop()
        }
    }
}
