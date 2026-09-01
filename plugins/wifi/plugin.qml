import QtQuick
import Qwin
import "../shared"

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

    Row {
        id: wifiRow
        anchors.centerIn: parent
        spacing: 6

        WifiIcon {
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

                WifiIcon {
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
                accentColor: Colors.accent
                surfaceColor: Colors.surface
                textColor: Colors.text
                onClicked: {
                    Wifi.openNetworkFlyout()
                    wifiMenu.dismiss()
                }
            }
        }
    }
}
