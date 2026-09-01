import QtQuick
import qwin 1.0
import "../shared"

// "Now Playing": play/pause, a "Title - Artist" line that marquees past
// maxWidth, and prev/next. The Media singleton follows whatever app owns
// Windows' now-playing state, so there is no per-app work here. Hides itself
// through `shown` when nothing is playing anywhere.
//
// config.json section (both keys optional):
//   "media": {
//       "maxWidth": 220,    // px before the title marquee-scrolls
//       "scrollSpeed": 30   // marquee speed in px/s
//   }
Rectangle {
    id: mediaItem
    property bool shown: Media.available
    visible: shown

    readonly property var cfg: Plugins.config("media")
    readonly property int maxWidth: cfg.maxWidth || 220
    readonly property real scrollSpeed: cfg.scrollSpeed || 30

    // Falls back to the source app's AUMID while a session exists but has
    // reported no metadata yet.
    function displayText() {
        if (Media.title.length === 0 && Media.artist.length === 0)
            return Media.sourceApp.length > 0 ? Media.sourceApp : "Now Playing"
        return Media.artist.length > 0 ? (Media.title + " - " + Media.artist) : Media.title
    }

    width: row.implicitWidth + 14
    height: 24
    radius: 5
    color: hoverArea.containsMouse ? Qt.alpha(Colors.surface, 0.13) : "transparent"

    // Hover highlight only - no popup here to also gate on. NoButton so it
    // never steals clicks from the MouseAreas inside `row`.
    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }

    // Canvas glyphs like battery's: no icon font ships media symbols.
    component TransportGlyph: Item {
        id: glyph
        width: 12
        height: 12
        property bool playing: false
        property color glyphColor: "white"

        onPlayingChanged: canvas.requestPaint()
        onGlyphColorChanged: canvas.requestPaint()

        Canvas {
            id: canvas
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = glyph.glyphColor
                const w = width, h = height
                if (glyph.playing) {
                    // Pause.
                    const barW = w * 0.28
                    ctx.fillRect(w * 0.14, 0, barW, h)
                    ctx.fillRect(w * 0.58, 0, barW, h)
                } else {
                    // Play.
                    ctx.beginPath()
                    ctx.moveTo(w * 0.15, 0)
                    ctx.lineTo(w * 0.15, h)
                    ctx.lineTo(w * 0.9, h * 0.5)
                    ctx.closePath()
                    ctx.fill()
                }
            }
        }
    }

    // Skip-next, mirrored horizontally for skip-previous rather than drawn
    // twice.
    component SkipGlyph: Item {
        id: sglyph
        width: 12
        height: 12
        property bool mirrored: false
        property color glyphColor: "white"

        onGlyphColorChanged: scanvas.requestPaint()
        onMirroredChanged: scanvas.requestPaint()

        Canvas {
            id: scanvas
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.save()
                if (sglyph.mirrored) {
                    ctx.translate(width, 0)
                    ctx.scale(-1, 1)
                }
                ctx.fillStyle = sglyph.glyphColor
                const w = width, h = height
                ctx.beginPath()
                ctx.moveTo(w * 0.1, 0)
                ctx.lineTo(w * 0.1, h)
                ctx.lineTo(w * 0.65, h * 0.5)
                ctx.closePath()
                ctx.fill()
                ctx.fillRect(w * 0.7, 0, w * 0.2, h)
                ctx.restore()
            }
        }
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6

        TransportGlyph {
            id: playGlyph
            anchors.verticalCenter: parent.verticalCenter
            playing: Media.playing
            glyphColor: Media.canPlayPause ? Colors.text : Qt.alpha(Colors.textMuted, 0.35)

            MouseArea {
                anchors.fill: parent
                anchors.margins: -3 // a slightly larger hit target
                enabled: Media.canPlayPause
                onClicked: Media.playPause()
            }
        }

        Item {
            id: titleViewport
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(mediaItem.maxWidth, titleText.implicitWidth)
            height: titleText.implicitHeight
            clip: true

            // Only while there is something to reveal and the plugin is on
            // screen and playing - an idle bar must not repaint forever.
            readonly property bool needsScroll: mediaItem.visible && Media.playing
                                                && titleText.implicitWidth > width
            // px/s -> ms, recomputed whenever the track changes.
            readonly property int scrollDurationMs: Math.max(400,
                (titleText.implicitWidth - width) / mediaItem.scrollSpeed * 1000)

            Text {
                id: titleText
                text: mediaItem.displayText()
                color: Colors.text
                font.family: Theme.fontFamily
                font.pixelSize: 13
            }

            SequentialAnimation {
                id: marquee
                loops: Animation.Infinite
                running: titleViewport.needsScroll
                // Reset too: stopping mid-scroll would otherwise leave the
                // text offset for good once it fits again.
                onRunningChanged: if (!running) titleText.x = 0
                PauseAnimation { duration: 900 }
                NumberAnimation {
                    target: titleText; property: "x"
                    to: -(titleText.implicitWidth - titleViewport.width)
                    duration: titleViewport.scrollDurationMs
                    easing.type: Easing.Linear
                }
                PauseAnimation { duration: 900 }
                NumberAnimation {
                    target: titleText; property: "x"
                    to: 0
                    duration: titleViewport.scrollDurationMs
                    easing.type: Easing.Linear
                }
            }

            MouseArea {
                anchors.fill: parent
                onClicked: Media.playPause()
            }
        }

        SkipGlyph {
            anchors.verticalCenter: parent.verticalCenter
            mirrored: true
            glyphColor: Media.canGoPrevious ? Colors.text : Qt.alpha(Colors.textMuted, 0.35)

            MouseArea {
                anchors.fill: parent
                anchors.margins: -3
                enabled: Media.canGoPrevious
                onClicked: Media.previous()
            }
        }

        SkipGlyph {
            anchors.verticalCenter: parent.verticalCenter
            glyphColor: Media.canGoNext ? Colors.text : Qt.alpha(Colors.textMuted, 0.35)

            MouseArea {
                anchors.fill: parent
                anchors.margins: -3
                enabled: Media.canGoNext
                onClicked: Media.next()
            }
        }
    }
}
