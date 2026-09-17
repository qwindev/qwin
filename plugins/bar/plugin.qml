import QtQuick
import Qwin

// One bar per monitor: a windowless Service that instantiates a PanelWindow
// per entry of System.screens (not Qt.application.screens - see
// System.screens' doc comment for why), each docked to its own monitor via
// PanelWindow.screenName. Primary first; the primary and secondary monitors'
// bars have separately configured module lists, picked per delegate by
// `modelData.primary`.
//
// config.json section:
//   "bar": {
//       "primary":   { "left": [...], "center": [...], "right": [...] },
//       "secondary": { "left": [...], "center": [...], "right": [...] }
//   }
//
// - `primary` missing falls back to a legacy top-level "left"/"center"/
//   "right" (existing configs from before per-monitor bars keep working
//   unchanged).
// - `secondary` missing gets a built-in slim default:
//   { "left": ["workspaces", "tiling-indicator", "window-title"] }.
// - `secondary: false` means no bar at all on a non-primary monitor.
// - Missing slots (in either section) are empty lists.
//
// Each name resolves through the Plugins registry into a Loader; the modules
// are ordinary plugins in sibling folders, left out of "enabled" so they
// appear only here. `plugins/workspaces`, `plugins/tiling-indicator` and
// `plugins/window-title` read their own monitor's device off
// `Window.window.device` (set by PanelWindow.screenName below), so the same
// module embedded on two different bars describes two different monitors.
Service {
    id: bar

    // Read once per load; a config.json edit reloads the plugin anyway.
    readonly property var cfg: Plugins.config("bar")

    function listsFor(section) {
        return { left: section.left || [], center: section.center || [], right: section.right || [] }
    }

    // No "primary" key -> the legacy shape lived at the top level of the
    // section itself, so reading left/center/right straight off `cfg` is the
    // fallback.
    readonly property var primaryLists: listsFor(cfg.primary !== undefined ? cfg.primary : cfg)

    readonly property bool secondaryEnabled: cfg.secondary !== false
    readonly property var secondaryLists: listsFor(
        cfg.secondary !== undefined ? cfg.secondary
                                     : { left: ["workspaces", "tiling-indicator", "window-title"] })

    // Primary monitor always gets a bar; the rest only when secondary bars
    // are not turned off entirely.
    readonly property var barScreens: System.screens.filter(function (s) {
        return s.primary || bar.secondaryEnabled
    })

    component Section: Row {
        id: section
        property var names: []
        spacing: 16

        Repeater {
            model: section.names
            Loader {
                required property var modelData
                anchors.verticalCenter: parent.verticalCenter
                source: Plugins.source(modelData)
                // Modules that can have nothing to report hide themselves
                // through `shown`, and the slot collapses with no Row gap.
                // Binding on item.visible instead deadlocks: it reads
                // EFFECTIVE visibility, which follows this Loader's own.
                visible: item !== null && (item.shown === undefined ? true : item.shown)
            }
        }
    }

    Instantiator {
        model: bar.barScreens

        PanelWindow {
            id: panel
            required property var modelData
            screenName: modelData.device
            edge: Qt.TopEdge
            thickness: 36
            visible: true
            // Colors come from colors.json via the Colors singleton, live.
            color: Colors.background

            readonly property var lists: modelData.primary ? bar.primaryLists : bar.secondaryLists

            Section {
                names: panel.lists.left
                anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
            }

            Section {
                names: panel.lists.center
                anchors { horizontalCenter: parent.horizontalCenter; verticalCenter: parent.verticalCenter }
            }

            Section {
                names: panel.lists.right
                anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
            }

            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: 1
                color: Qt.alpha(Colors.accent, 0.2)
            }
        }
    }
}
