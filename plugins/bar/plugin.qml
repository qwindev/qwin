import QtQuick
import qwin 1.0

// Top panel: a thin PanelWindow whose content is assembled at runtime from
// the plugin names its config.json section lists per slot:
//
//   "bar": { "left": ["workspaces"], "center": ["clock"],
//            "right": ["wifi", "system-stats"] }
//
// Each name resolves through the Plugins registry into a Loader. The modules
// are ordinary plugins in sibling folders, left out of "enabled" so they
// appear only here; editing config.json rearranges the bar live.
PanelWindow {
    id: bar
    edge: Qt.TopEdge
    thickness: 36
    visible: true
    // Colors come from colors.json via the Colors singleton, live.
    color: Colors.background

    // Read once per load; a config.json edit triggers a full reload anyway.
    readonly property var cfg: Plugins.config("bar")

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

    Section {
        names: bar.cfg.left || []
        anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
    }

    Section {
        names: bar.cfg.center || []
        anchors { horizontalCenter: parent.horizontalCenter; verticalCenter: parent.verticalCenter }
    }

    Section {
        names: bar.cfg.right || []
        anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
    }

    Rectangle {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 1
        color: Qt.alpha(Colors.accent, 0.2)
    }
}
