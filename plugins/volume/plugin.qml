import QtQuick
import qwin 1.0
import "../shared"

// Volume and output-device switcher: speaker glyph and percent in the bar,
// wheel to nudge the level, middle-click to mute without opening anything.
// The popup adds a slider, a mute toggle and the active output devices, a
// row per Audio.setDefaultDevice() target. Hides itself with no output
// device: `shown` for the bar's Loader, `visible` for standalone use.
//
// config.json section (all keys optional):
//   "volume": {
//       "step": 5,            // volume change per wheel notch
//       "showPercent": true   // the percent text next to the glyph
//   }
Rectangle {
    id: volumeItem
    property bool shown: Audio.available
    visible: shown

    readonly property var cfg: Plugins.config("volume")
    readonly property int step: cfg.step || 5
    readonly property bool showPercent: cfg.showPercent === false ? false : true

    // Muted or silent reads muted, like the battery glyph's "nothing to
    // report"; any audible level reads as normal text.
    readonly property color glyphColor: (Audio.muted || Audio.volume <= 0) ? Colors.textMuted : Colors.text

    // A constant 4 characters ("  0%", " 45%", "100%", "MUTE"), so the bar
    // does not reflow at a digit boundary.
    function percentText() {
        return (Audio.muted ? "mute" : (Audio.volume + "%")).padStart(4)
    }

    width: volumeRow.implicitWidth + 14
    height: 24
    radius: 5
    color: volumeMouse.containsMouse || volumeMenu.opened ? Qt.alpha(Colors.surface, 0.13)
                                                           : "transparent"

    // Speaker glyph: body and cone as one filled path, then zero to two
    // stroked arcs for the sound waves, or a slash when muted. Inline like
    // BatteryGlyph, since nothing else needs it.
    component SpeakerGlyph: Item {
        id: glyph
        width: 16
        height: 14

        property int level: 0 // 0-100
        property bool muted: false
        property color glyphColor: "white"

        onLevelChanged: canvas.requestPaint()
        onMutedChanged: canvas.requestPaint()
        onGlyphColorChanged: canvas.requestPaint()

        Canvas {
            id: canvas
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const w = width
                const h = height
                ctx.fillStyle = glyph.glyphColor
                ctx.strokeStyle = glyph.glyphColor
                ctx.lineWidth = 1.3
                ctx.lineJoin = "round"
                ctx.lineCap = "round"

                // Body and cone as one filled path.
                const bodyW = w * 0.32
                const bodyTop = h * 0.32
                const bodyBottom = h * 0.68
                ctx.beginPath()
                ctx.moveTo(0, bodyTop)
                ctx.lineTo(bodyW, bodyTop)
                ctx.lineTo(w * 0.58, h * 0.08)
                ctx.lineTo(w * 0.58, h * 0.92)
                ctx.lineTo(bodyW, bodyBottom)
                ctx.lineTo(0, bodyBottom)
                ctx.closePath()
                ctx.fill()

                if (glyph.muted) {
                    // Replaces the arcs; both together read as contradictory.
                    ctx.beginPath()
                    ctx.moveTo(w * 0.66, h * 0.12)
                    ctx.lineTo(w * 0.98, h * 0.88)
                    ctx.stroke()
                    return
                }

                const cx = w * 0.58
                const cy = h * 0.5
                if (glyph.level > 0) {
                    ctx.beginPath()
                    ctx.arc(cx, cy, w * 0.16, -Math.PI / 4, Math.PI / 4)
                    ctx.stroke()
                }
                if (glyph.level > 50) {
                    ctx.beginPath()
                    ctx.arc(cx, cy, w * 0.30, -Math.PI / 3.2, Math.PI / 3.2)
                    ctx.stroke()
                }
            }
        }
    }

    Row {
        id: volumeRow
        anchors.centerIn: parent
        spacing: 6

        SpeakerGlyph {
            anchors.verticalCenter: parent.verticalCenter
            level: Audio.volume
            muted: Audio.muted
            glyphColor: volumeItem.glyphColor
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: volumeItem.showPercent
            text: volumeItem.percentText()
            color: volumeItem.glyphColor
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }
    }

    MouseArea {
        id: volumeMouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        onClicked: mouse => {
            if (mouse.button === Qt.MiddleButton)
                Audio.toggleMute()
            else
                volumeMenu.toggle()
        }
        onWheel: wheel => {
            Audio.adjustVolume(wheel.angleDelta.y > 0 ? volumeItem.step : -volumeItem.step)
        }
    }

    Popup {
        id: volumeMenu
        anchorItem: volumeItem
        contentWidth: 260
        contentHeight: volumeMenuColumn.implicitHeight + 24
        backgroundColor: Colors.background
        borderColor: Qt.alpha(Colors.accent, 0.2)

        Column {
            id: volumeMenuColumn
            width: parent.width
            spacing: 12

            Row {
                spacing: 8

                SpeakerGlyph {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 20; height: 17
                    level: Audio.volume
                    muted: Audio.muted
                    glyphColor: Audio.muted ? Colors.textMuted : Colors.accent
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: Audio.muted ? "Muted" : (Audio.volume + "%")
                    color: Colors.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 22
                    font.bold: true
                }
            }

            Text {
                text: Audio.deviceName.length > 0 ? Audio.deviceName : "No output device"
                color: Colors.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 12
                elide: Text.ElideRight
                width: parent.width
            }

            // Track and handle are cosmetic: one MouseArea spans the whole
            // taller item, so a click anywhere and a drag from the handle
            // both work without two MouseAreas fighting for the same pixels.
            Item {
                id: sliderRoot
                width: parent.width
                height: 20

                readonly property real ratio: Audio.volume / 100

                Rectangle {
                    id: sliderTrack
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 4
                    radius: 2
                    color: Qt.alpha(Colors.textMuted, 0.3)

                    Rectangle {
                        width: sliderTrack.width * sliderRoot.ratio
                        height: parent.height
                        radius: 2
                        color: Audio.muted ? Colors.textMuted : Colors.accent
                    }
                }

                Rectangle {
                    width: 12
                    height: 12
                    radius: 6
                    color: Colors.text
                    anchors.verticalCenter: parent.verticalCenter
                    x: sliderTrack.width * sliderRoot.ratio - width / 2
                }

                MouseArea {
                    anchors.fill: parent
                    function applyFromX(mx) {
                        const ratio = Math.max(0, Math.min(1, mx / sliderRoot.width))
                        Audio.setVolume(Math.round(ratio * 100))
                    }
                    onPressed: mouse => applyFromX(mouse.x)
                    onPositionChanged: mouse => { if (pressed) applyFromX(mouse.x) }
                }
            }

            // The same hand-off button the wifi and battery popups use.
            PopupButton {
                width: parent.width
                label: Audio.muted ? "Unmute" : "Mute"
                accentColor: Colors.accent
                surfaceColor: Colors.surface
                textColor: Colors.text
                onClicked: Audio.toggleMute()
            }

            Text {
                text: "Output device"
                color: Colors.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 11
            }

            Column {
                width: parent.width
                spacing: 2

                Repeater {
                    model: Audio.devices

                    Rectangle {
                        id: deviceRow
                        required property var modelData
                        width: parent.width
                        height: 28
                        radius: 6
                        color: deviceMouse.containsMouse && Audio.canSwitchDevices
                               ? Qt.alpha(Colors.accent, 0.18) : "transparent"

                        Row {
                            anchors.left: parent.left
                            anchors.leftMargin: 6
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6

                            Rectangle {
                                width: 6; height: 6; radius: 3
                                anchors.verticalCenter: parent.verticalCenter
                                color: Colors.accent
                                // Reserved either way, so the names line up.
                                opacity: deviceRow.modelData.isDefault ? 1 : 0
                            }

                            Text {
                                text: deviceRow.modelData.name
                                color: deviceRow.modelData.isDefault ? Colors.accent : Colors.text
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                elide: Text.ElideRight
                                width: parent.width - 12
                            }
                        }

                        MouseArea {
                            id: deviceMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: Audio.canSwitchDevices && !deviceRow.modelData.isDefault
                            onClicked: Audio.setDefaultDevice(deviceRow.modelData.id)
                        }
                    }
                }

                // With IPolicyConfig gone (see audioapi.cpp) the rows above
                // are read-only; say why rather than leave them silently
                // unclickable.
                Text {
                    visible: !Audio.canSwitchDevices
                    width: parent.width
                    text: "Device switching is unavailable on this system - use Sound settings below."
                    color: Colors.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }
            }

            PopupButton {
                width: parent.width
                label: "Sound settings"
                accentColor: Colors.accent
                surfaceColor: Colors.surface
                textColor: Colors.text
                onClicked: {
                    Audio.openSoundSettings()
                    volumeMenu.dismiss()
                }
            }
        }
    }
}
