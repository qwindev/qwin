import QtQuick
import "."

// The shared popup action button - a bordered, hoverable row used for a
// popup's single hand-off action (e.g. "Power settings", "Switch network").
// Colors arrive as properties, like the other shared components.
Rectangle {
    id: button

    property string label
    property color accentColor: "#55D6C2"
    property color surfaceColor: "#455055"
    property color textColor: "white"

    signal clicked()

    height: 32
    radius: 8
    color: mouse.containsMouse ? Qt.alpha(button.accentColor, 0.25)
                                : Qt.alpha(button.surfaceColor, 0.13)
    border.color: Qt.alpha(button.accentColor, 0.4)
    border.width: 1

    Text {
        anchors.centerIn: parent
        text: button.label
        color: button.textColor
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
