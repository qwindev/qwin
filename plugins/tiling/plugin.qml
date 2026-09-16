import QtQuick
import Qwin

// Tiling: a windowless Service that pushes config into the `Tiler` singleton
// and binds every tiling and workspace chord, so it goes in "enabled" rather
// than in the bar. The layout itself lives in C++ and survives hot reloads -
// reloading this never disturbs windows already placed. The bar's
// `tiling-indicator` and `workspaces` modules only display state.
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
//       "enabled": true,        // false loads config and chords with tiling
//                                // off until the indicator or toggle chord
//                                // turns it on
//       "gap": 16,               // between windows, logical px
//       "outerGap": 16,          // to the edge of the work area
//       "minWidth": 360,          // smallest tile a split may create
//       "minHeight": 220,         // below either, the new window stays floating
//       "resizeStep": 40,         // px one grow/shrink chord moves a divider
//       "floatProcesses": ["Taskmgr.exe"],
//       "workspaces": 9,         // per monitor, 1-20; see plugins/workspaces
//       "hideMethod": "cloak",   // "cloak" | "minimize"; see docs/api.md#tiler
//       "pinnedTopmost": false,  // keep a pinned window above the active workspace
//       "debug": false,          // log every adoption and every rect applied
//       "keys": {
//           "focusLeft": "Alt+H", ...,          // see defaults below
//           "switchWorkspace": "Shift+Alt+{n}", // {n} -> 1..min(Tiler.workspaceCount, 9)
//           "moveToWorkspace": "Ctrl+Alt+{n}",  // moves the foreground window, does not follow it
//           "moveToEmptyWorkspace": "Shift+Alt+M" // window -> first empty workspace, and follow
//       }
//   }
Service {
    id: tiling

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
        { seq: chord("focusLeft",      "Alt+H"),       action: "focus",  arg: "left",     desc: "Focus window left"      },
        { seq: chord("focusDown",      "Alt+J"),       action: "focus",  arg: "down",     desc: "Focus window down"      },
        { seq: chord("focusUp",        "Alt+K"),       action: "focus",  arg: "up",       desc: "Focus window up"        },
        { seq: chord("focusRight",     "Alt+L"),       action: "focus",  arg: "right",    desc: "Focus window right"     },
        { seq: chord("moveLeft",       "Shift+Alt+H"), action: "move",   arg: "left",     desc: "Move window left"       },
        { seq: chord("moveDown",       "Shift+Alt+J"), action: "move",   arg: "down",     desc: "Move window down"       },
        { seq: chord("moveUp",         "Shift+Alt+K"), action: "move",   arg: "up",       desc: "Move window up"         },
        { seq: chord("moveRight",      "Shift+Alt+L"), action: "move",   arg: "right",    desc: "Move window right"      },
        { seq: chord("shrinkWidth",    "Ctrl+Alt+H"),  action: "resize", arg: "narrower", desc: "Shrink width"           },
        { seq: chord("growHeight",     "Ctrl+Alt+J"),  action: "resize", arg: "taller",   desc: "Grow height"            },
        { seq: chord("shrinkHeight",   "Ctrl+Alt+K"),  action: "resize", arg: "shorter",  desc: "Shrink height"          },
        { seq: chord("growWidth",      "Ctrl+Alt+L"),  action: "resize", arg: "wider",    desc: "Grow width"             },
        { seq: chord("toggleFloating", "Shift+Alt+F"), action: "float",                   desc: "Toggle floating"        },
        { seq: chord("toggleSplit",    "Shift+Alt+V"), action: "split",                   desc: "Toggle split direction" },
        { seq: chord("equalize",       "Shift+Alt+E"), action: "equalize",                desc: "Equalize splits"        },
        { seq: chord("toggleTiling",   "Shift+Alt+T"), action: "toggle",                  desc: "Toggle tiling"          },
        { seq: chord("togglePinned",   "Shift+Alt+P"), action: "pin",                     desc: "Toggle pinned"          },
        { seq: chord("moveToEmptyWorkspace", "Shift+Alt+M"), action: "moveToEmpty",       desc: "Move window to empty workspace" }
    ]

    // A name and a switch rather than a closure per row: the model is data,
    // and a bad config key then warns instead of failing silently.
    function run(action, arg) {
        switch (action) {
        case "focus":       Tiler.focusDirection(arg); break
        case "move":        Tiler.moveDirection(arg); break
        case "resize":      Tiler.resize(arg); break
        case "float":       Tiler.toggleFloating(); break
        case "split":       Tiler.toggleSplit(); break
        case "equalize":    Tiler.equalize(); break
        case "toggle":      Tiler.enabled = !Tiler.enabled; break
        case "pin":         Tiler.togglePinned(); break
        case "moveToEmpty": Tiler.moveToEmptyWorkspace(); break
        default: console.warn("tiling: unknown action", action)
        }
    }

    // Config-driven and never touched at runtime, so a plain binding is
    // right. `enabled` is not among them - the toggle chord and the
    // indicator's click own it after load, and a binding would snap it back.
    Binding { target: Tiler; property: "gap"; value: tiling.cfg.gap !== undefined ? tiling.cfg.gap : 16 }
    Binding { target: Tiler; property: "outerGap"; value: tiling.cfg.outerGap !== undefined ? tiling.cfg.outerGap : 16 }
    Binding { target: Tiler; property: "minWidth"; value: tiling.cfg.minWidth !== undefined ? tiling.cfg.minWidth : 360 }
    Binding { target: Tiler; property: "minHeight"; value: tiling.cfg.minHeight !== undefined ? tiling.cfg.minHeight : 220 }
    Binding { target: Tiler; property: "resizeStep"; value: tiling.cfg.resizeStep !== undefined ? tiling.cfg.resizeStep : 40 }
    Binding { target: Tiler; property: "floatProcesses"; value: tiling.cfg.floatProcesses || [] }
    Binding { target: Tiler; property: "workspaceCount"; value: tiling.cfg.workspaces !== undefined ? tiling.cfg.workspaces : 9 }
    Binding { target: Tiler; property: "hideMethod"; value: tiling.cfg.hideMethod !== undefined ? tiling.cfg.hideMethod : "cloak" }
    Binding { target: Tiler; property: "pinnedTopmost"; value: tiling.cfg.pinnedTopmost === true }
    Binding { target: Tiler; property: "debug"; value: tiling.cfg.debug === true }

    // Loading this plugin is the opt-in, so a missing key means on.
    Component.onCompleted: Tiler.enabled = tiling.cfg.enabled !== false

    Instantiator {
        model: tiling.bindings

        Hotkey {
            required property var modelData
            sequence: modelData.seq
            description: modelData.desc
            onActivated: tiling.run(modelData.action, modelData.arg)
        }
    }

    // Shift+Alt+1..9 by default: switch the focused monitor to that
    // workspace. Capped at 9 - there is no single key left beyond that.
    Instantiator {
        model: Math.min(Tiler.workspaceCount, 9)

        Hotkey {
            required property int index
            // index goes -1 while the Instantiator tears an item down; an
            // empty sequence stops it re-registering on a stale number.
            sequence: index >= 0 ? tiling.chord("switchWorkspace", "Shift+Alt+{n}").replace("{n}", index + 1) : ""
            description: index >= 0 ? "Switch to workspace " + (index + 1) : ""
            onActivated: Tiler.switchToWorkspace(index)
        }
    }

    // Ctrl+Alt+1..9 by default: send the foreground window to that
    // workspace without following it. A hotkey, not a button: a chord does
    // not change focus, so the foreground window is still the one the user
    // was in.
    Instantiator {
        model: Math.min(Tiler.workspaceCount, 9)

        Hotkey {
            required property int index
            sequence: index >= 0 ? tiling.chord("moveToWorkspace", "Ctrl+Alt+{n}").replace("{n}", index + 1) : ""
            description: index >= 0 ? "Move window to workspace " + (index + 1) : ""
            onActivated: Tiler.moveToWorkspace(index)
        }
    }
}
