import QtQuick
import qwin 1.0
import "../shared"

// Tiling: the keyboard surface for the `Tiler` singleton, plus a bar
// indicator. The layout itself lives in C++ and survives hot reloads - this
// plugin only pushes config into it and binds chords to its commands, so
// reloading never disturbs windows already placed.
//
// Windows are arranged dwindle-style: each new window splits the focused
// one along its longer side. Drag any inner edge and the split behind it is
// rewritten, so the grid stays a grid. Fixed-size dialogs are never tiled.
//
// The resize chords do by keyboard what dragging an edge does: they move the
// divider nearest the focused window, so which of its edges travels depends
// on where it sits in the partition.
//
// config.json section (all keys optional):
//   "tiling": {
//       "enabled": true,
//       "gap": 16,               // between windows, logical px
//       "outerGap": 16,          // to the edge of the work area
//       "minWidth": 360,          // smallest tile a split may create
//       "minHeight": 220,         // below either, the new window stays floating
//       "resizeStep": 40,         // px one grow/shrink chord moves a divider
//       "floatProcesses": ["Taskmgr.exe"],
//       "debug": false,          // log every adoption and every rect applied
//       "keys": { "focusLeft": "Alt+H", ... }   // see defaults below
//   }
Rectangle {
    id: tilingItem

    // Read once per load; a config.json edit reloads the plugin anyway.
    readonly property var cfg: Plugins.config("tiling")
    readonly property var keyCfg: cfg.keys || ({})

    function chord(name, fallback) {
        return keyCfg[name] !== undefined ? keyCfg[name] : fallback
    }

    // Vim directions by default. Arrows would be the obvious choice but
    // Alt+Left/Right is browser back/forward, and a global chord wins over
    // the focused app - the tiler would eat it everywhere.
    readonly property var bindings: [
        { seq: chord("focusLeft",      "Alt+H"),       action: "focus",  arg: "left"  },
        { seq: chord("focusDown",      "Alt+J"),       action: "focus",  arg: "down"  },
        { seq: chord("focusUp",        "Alt+K"),       action: "focus",  arg: "up"    },
        { seq: chord("focusRight",     "Alt+L"),       action: "focus",  arg: "right" },
        { seq: chord("moveLeft",       "Shift+Alt+H"), action: "move",   arg: "left"  },
        { seq: chord("moveDown",       "Shift+Alt+J"), action: "move",   arg: "down"  },
        { seq: chord("moveUp",         "Shift+Alt+K"), action: "move",   arg: "up"    },
        { seq: chord("moveRight",      "Shift+Alt+L"), action: "move",   arg: "right" },
        { seq: chord("shrinkWidth",    "Ctrl+Alt+H"),  action: "resize", arg: "narrower" },
        { seq: chord("growHeight",     "Ctrl+Alt+J"),  action: "resize", arg: "taller"   },
        { seq: chord("shrinkHeight",   "Ctrl+Alt+K"),  action: "resize", arg: "shorter"  },
        { seq: chord("growWidth",      "Ctrl+Alt+L"),  action: "resize", arg: "wider"    },
        { seq: chord("toggleFloating", "Shift+Alt+F"), action: "float"    },
        { seq: chord("toggleSplit",    "Shift+Alt+V"), action: "split"    },
        { seq: chord("equalize",       "Shift+Alt+E"), action: "equalize" },
        { seq: chord("toggleTiling",   "Shift+Alt+T"), action: "toggle"   }
    ]

    // A name and a switch rather than a closure per row: the model is data,
    // and a bad config key then warns instead of failing silently.
    function run(action, arg) {
        switch (action) {
        case "focus":    Tiler.focusDirection(arg); break
        case "move":     Tiler.moveDirection(arg); break
        case "resize":   Tiler.resize(arg); break
        case "float":    Tiler.toggleFloating(); break
        case "split":    Tiler.toggleSplit(); break
        case "equalize": Tiler.equalize(); break
        case "toggle":   Tiler.enabled = !Tiler.enabled; break
        default: console.warn("tiling: unknown action", action)
        }
    }

    // Config-driven and never touched at runtime, so a plain binding is
    // right. `enabled` is not among them - the chord and the click below
    // own it, and a binding would snap it back.
    Binding { target: Tiler; property: "gap"; value: tilingItem.cfg.gap !== undefined ? tilingItem.cfg.gap : 16 }
    Binding { target: Tiler; property: "outerGap"; value: tilingItem.cfg.outerGap !== undefined ? tilingItem.cfg.outerGap : 16 }
    Binding { target: Tiler; property: "minWidth"; value: tilingItem.cfg.minWidth !== undefined ? tilingItem.cfg.minWidth : 360 }
    Binding { target: Tiler; property: "minHeight"; value: tilingItem.cfg.minHeight !== undefined ? tilingItem.cfg.minHeight : 220 }
    Binding { target: Tiler; property: "resizeStep"; value: tilingItem.cfg.resizeStep !== undefined ? tilingItem.cfg.resizeStep : 40 }
    Binding { target: Tiler; property: "floatProcesses"; value: tilingItem.cfg.floatProcesses || [] }
    Binding { target: Tiler; property: "debug"; value: tilingItem.cfg.debug === true }

    Component.onCompleted: {
        if (tilingItem.cfg.enabled !== undefined)
            Tiler.enabled = tilingItem.cfg.enabled
    }

    Instantiator {
        model: tilingItem.bindings

        Hotkey {
            required property var modelData
            sequence: modelData.seq
            onActivated: tilingItem.run(modelData.action, modelData.arg)
        }
    }

    width: row.implicitWidth + 14
    height: 24
    radius: 5
    color: Tiler.enabled ? Qt.alpha(Colors.accent, mouse.containsMouse ? 0.26 : 0.15)
                         : (mouse.containsMouse ? Qt.alpha(Colors.surface, 0.13) : "transparent")

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6

        // The dwindle partition itself as the glyph: one vertical divider,
        // then a horizontal one in the smaller half. Filled panes while
        // tiling is on, outline while off - a shape difference, not just a
        // colour shift, so it reads at bar size.
        Canvas {
            id: glyph
            width: 16
            height: 14
            anchors.verticalCenter: parent.verticalCenter

            readonly property bool active: Tiler.enabled
            readonly property color tint: active ? Colors.accent : Colors.textMuted

            onActiveChanged: requestPaint()
            onTintChanged: requestPaint()

            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = glyph.tint
                ctx.fillStyle = glyph.tint
                ctx.lineWidth = 1.2

                const split = Math.round(width * 0.55)
                const half = Math.round(height * 0.5)
                const panes = [
                    Qt.rect(0.6, 0.6, split - 1.8, height - 1.2),
                    Qt.rect(split + 0.6, 0.6, width - split - 1.2, half - 1.2),
                    Qt.rect(split + 0.6, half + 0.6, width - split - 1.2, height - half - 1.2)
                ]
                for (let i = 0; i < panes.length; i++) {
                    const p = panes[i]
                    ctx.beginPath()
                    ctx.rect(p.x, p.y, p.width, p.height)
                    if (glyph.active)
                        ctx.fill()
                    else
                        ctx.stroke()
                }
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: Tiler.managedCount
            color: Tiler.enabled ? Colors.accent : Colors.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: Tiler.enabled = !Tiler.enabled
    }
}
