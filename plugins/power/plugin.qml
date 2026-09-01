pragma ComponentBehavior: Bound
import QtQuick
import qwin 1.0
import "../shared"

// Power: the session menu over the Power singleton, which acts immediately.
// The confirmation is here instead: every row but Lock arms on the first
// click ("Click again to confirm") and acts on a second within
// confirmSeconds, with hovering away, closing the popup or the timeout
// cancelling. Restart and Shut down arm in Colors.error, the rest in
// Colors.warning. Lock skips the step - it is reversible in a way none of
// the others are.
//
// config.json section (all keys optional):
//   "power": {
//       "confirmSeconds": 4,   // arm window
//       "items": ["lock","signout","sleep","hibernate","restart","shutdown"]
//   }
Rectangle {
    id: powerItem

    readonly property var cfg: Plugins.config("power")
    readonly property int confirmMs: (cfg.confirmSeconds || 4) * 1000
    readonly property var itemsCfg: cfg.items
                                     || ["lock", "signout", "sleep", "hibernate", "restart", "shutdown"]

    readonly property var allRows: [
        { key: "lock", label: "Lock" },
        { key: "signout", label: "Sign out" },
        { key: "sleep", label: "Sleep" },
        { key: "hibernate", label: "Hibernate" },
        { key: "restart", label: "Restart" },
        { key: "shutdown", label: "Shut down" }
    ]
    // Hibernate is filtered by availability on top of the config list, so a
    // machine with it off never offers a row that would just fail.
    readonly property var visibleRows: allRows.filter(function (r) {
        return itemsCfg.indexOf(r.key) !== -1 && (r.key !== "hibernate" || Power.hibernateAvailable)
    })

    width: row.implicitWidth + 14
    height: 24
    radius: 5
    color: mouse.containsMouse || menu.opened ? Qt.alpha(Colors.surface, 0.13) : "transparent"

    // The standard circle-with-a-gap plus a stroke through the gap.
    component PowerGlyph: Item {
        id: glyph
        width: 16
        height: 16
        property color glyphColor: "white"

        onGlyphColorChanged: canvas.requestPaint()

        Canvas {
            id: canvas
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const cx = width / 2
                const cy = height / 2
                const r = Math.min(width, height) * 0.36

                ctx.strokeStyle = glyph.glyphColor
                ctx.lineWidth = 1.6
                ctx.lineCap = "round"

                // Gap centered on 12 o'clock, for the stroke below.
                const gap = Math.PI * 0.24
                ctx.beginPath()
                ctx.arc(cx, cy, r, -Math.PI / 2 + gap / 2, Math.PI * 1.5 - gap / 2)
                ctx.stroke()

                // Poking above the circle.
                ctx.beginPath()
                ctx.moveTo(cx, cy - r - 1.5)
                ctx.lineTo(cx, cy - r * 0.1)
                ctx.stroke()
            }
        }
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6

        PowerGlyph {
            anchors.verticalCenter: parent.verticalCenter
            width: 15
            height: 15
            glyphColor: mouse.containsMouse || menu.opened ? Colors.text : Colors.textMuted
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: menu.toggle()
    }

    Popup {
        id: menu
        anchorItem: powerItem
        contentWidth: 220
        contentHeight: menuColumn.implicitHeight + 24
        backgroundColor: Colors.background
        borderColor: Qt.alpha(Colors.accent, 0.2)

        // Connections, not an `onOpenedChanged:` here: Popup.qml's root
        // already owns onVisibleChanged for the focus handover, and a second
        // assignment would replace it rather than add to it.
        Connections {
            target: menu
            function onOpenedChanged() {
                if (!menu.opened)
                    menuColumn.disarm()
            }
        }

        Column {
            id: menuColumn
            width: parent.width
            spacing: 2

            // The row awaiting confirmation, or "". One key rather than a
            // per-row flag is what makes arming a row cancel the last.
            property string armedKey: ""

            // Driven by arm()/disarm(), not a `running: armedKey !== ""`
            // binding: that stays true across a switch between armed rows,
            // so the new one would inherit the remains of the old window.
            Timer {
                id: armTimer
                interval: powerItem.confirmMs
                onTriggered: menuColumn.armedKey = ""
            }

            function arm(key) {
                armedKey = key
                armTimer.restart()
            }

            function disarm() {
                armedKey = ""
                armTimer.stop()
            }

            function handleRowClick(key) {
                if (key === "lock") {
                    menu.dismiss()
                    Power.lock()
                    return
                }
                if (armedKey === key) {
                    disarm()
                    // Dismiss before acting: an always-on-top popup left up
                    // while the session goes down reads as broken.
                    menu.dismiss()
                    switch (key) {
                    case "signout": Power.signOut(); break
                    case "sleep": Power.sleep(); break
                    case "hibernate": Power.hibernate(); break
                    case "restart": Power.restart(); break
                    case "shutdown": Power.shutdown(); break
                    }
                } else {
                    arm(key)
                }
            }

            // Inline rather than a reusable `component`, so under `pragma
            // ComponentBehavior: Bound` it stays in this document's id scope
            // and can read menuColumn directly.
            Repeater {
                model: powerItem.visibleRows

                Rectangle {
                    id: rowItem
                    required property var modelData
                    readonly property bool danger: modelData.key === "restart" || modelData.key === "shutdown"
                    readonly property bool armed: menuColumn.armedKey === modelData.key

                    width: parent.width
                    height: 26
                    radius: 6
                    // The wifi/battery popup button, minus its permanent
                    // border: six bordered rows stacked read too heavy. The
                    // border returns in the danger colour while armed, as the
                    // "confirm this" cue.
                    color: rowMouse.containsMouse ? Qt.alpha(Colors.accent, 0.18) : "transparent"
                    border.width: armed ? 1 : 0
                    border.color: danger ? Colors.error : Colors.warning

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: rowItem.armed ? "Click again to confirm" : rowItem.modelData.label
                        color: rowItem.armed ? (rowItem.danger ? Colors.error : Colors.warning) : Colors.text
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        elide: Text.ElideRight
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: menuColumn.handleRowClick(rowItem.modelData.key)
                        onExited: if (menuColumn.armedKey === rowItem.modelData.key) menuColumn.disarm()
                    }
                }
            }
        }
    }
}
