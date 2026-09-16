import QtQuick
import QtQuick.Window
import Qwin
import "../shared"

// Hotkey cheat sheet: a hotkey summons a centered overlay listing every
// `Hotkey` Qwin has declared - chord, owning plugin, description, and
// whether the OS actually granted it. Pulled from `Hotkeys.list()` fresh on
// every summon, so it is never stale even right after a hot reload changes
// what is registered.
//
// Sibling of `launcher` and `run`: same hotkey/centered-frame/filter/focus-
// bracket shape, over Hotkeys.list() instead of a link list or the app index.
//
// config.json section (all keys optional):
//   "hotkeys": {
//       "hotkey": "Shift+Alt+/",  // chord that summons it
//       "width": 560,             // frame width in pixels
//       "top": 0.2                // frame top, as a share of screen height
//   }
Window {
    id: win

    visible: false // the host leaves a deliberately hidden root hidden
    flags: Qt.FramelessWindowHint | Qt.Tool | Qt.WindowStaysOnTopHint
    color: "transparent"

    readonly property var cfg: Plugins.config("hotkeys")
    readonly property string hotkeySequence: cfg.hotkey || "Shift+Alt+/"
    readonly property int frameWidth: cfg.width || 560
    readonly property real topFraction: cfg.top || 0.2

    readonly property int pad: 16
    readonly property int inputHeight: 36
    readonly property int rowHeight: 26
    readonly property int headerHeight: 36
    readonly property int footerHeight: 24
    readonly property int chordColumnWidth: 130
    readonly property int scrollStep: rowHeight * 2

    // Fixed chrome (everything but the scrollable list), used both to size
    // the empty state and to cap how tall the list itself may grow.
    readonly property int chromeHeight: pad * 2 + inputHeight + 1 + footerHeight
    readonly property int maxListHeight: Math.max(rowHeight, Math.round(Screen.height * 0.7) - chromeHeight)
    readonly property int maxFrameHeight: chromeHeight + maxListHeight

    width: frameWidth
    height: chromeHeight + (hasResults ? listHeight : rowHeight)
    x: Screen.virtualX + (Screen.width - width) / 2

    // Pinned by its top edge, like `run`: the frame grows downward only,
    // clamped against the worst-case height rather than the live one so it
    // never jumps mid-filter.
    y: Screen.virtualY + Math.min(Math.round(Screen.height * topFraction),
                                  Math.max(0, Screen.height - maxFrameHeight))

    // ---- data -----------------------------------------------------------

    // A snapshot, not a binding: Hotkeys.list() is a call, refreshed only on
    // summon() - exactly what the singleton promises (see docs/api.md).
    property var entries: []

    readonly property string query: filterInput.text.trim().toLowerCase()

    function matches(e, q) {
        if (q === "")
            return true
        return e.sequence.toLowerCase().includes(q)
            || (e.plugin || "").toLowerCase().includes(q)
            || (e.description || "").toLowerCase().includes(q)
    }

    readonly property var filtered: entries.filter(e => win.matches(e, win.query))

    // entries (and therefore filtered) arrive sorted by plugin, so grouping
    // is a single pass rather than a sort.
    readonly property var groups: {
        const out = []
        let cur = null
        for (const e of win.filtered) {
            const name = e.plugin || "?"
            if (!cur || cur.plugin !== name) {
                cur = { plugin: name, entries: [] }
                out.push(cur)
            }
            cur.entries.push(e)
        }
        return out
    }

    readonly property bool hasResults: groups.length > 0
    readonly property real listHeight: Math.min(listColumn.height, maxListHeight)

    function scroll(delta) {
        flick.contentY = Math.max(0, Math.min(Math.max(0, flick.contentHeight - flick.height),
                                              flick.contentY + delta))
    }

    // ---- summoning --------------------------------------------------------

    Hotkey {
        sequence: win.hotkeySequence
        description: "Show hotkeys"
        onActivated: win.visible ? win.hide() : win.summon()
    }

    function summon() {
        entries = Hotkeys.list()
        filterInput.text = ""
        flick.contentY = 0
        System.rememberFocus()
        show()
        raise()
        requestActivate()
        filterInput.forceActiveFocus()
    }

    onActiveChanged: if (!active && visible) hide()

    // Alt+F4 dismisses like Esc, rather than destroying the window the
    // hotkey re-summons.
    onClosing: (close) => { close.accepted = false; win.hide() }

    onVisibleChanged: visible ? fadeIn.restart() : System.restoreFocus()
    NumberAnimation { id: fadeIn; target: win; property: "opacity"; from: 0; to: 1; duration: 110 }

    // ---- ui -----------------------------------------------------------------

    Rectangle {
        anchors.fill: parent
        radius: 12
        color: Qt.alpha(Colors.background, 0.96)
        border.color: Qt.alpha(Colors.accent, 0.35)
        border.width: 1
    }

    Item {
        id: search
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: win.pad }
        height: win.inputHeight

        TextInput {
            id: filterInput
            anchors {
                left: parent.left; leftMargin: 4
                right: counter.left; rightMargin: 12
                verticalCenter: parent.verticalCenter
            }
            color: Colors.text
            font.family: Theme.fontFamily
            font.pixelSize: 19
            selectionColor: Qt.alpha(Colors.accent, 0.35)
            selectedTextColor: Colors.text
            clip: true
            focus: true

            Keys.onPressed: (event) => {
                switch (event.key) {
                case Qt.Key_Up: win.scroll(-win.scrollStep); event.accepted = true; break
                case Qt.Key_Down: win.scroll(win.scrollStep); event.accepted = true; break
                case Qt.Key_PageUp: win.scroll(-flick.height); event.accepted = true; break
                case Qt.Key_PageDown: win.scroll(flick.height); event.accepted = true; break
                case Qt.Key_Escape: win.hide(); event.accepted = true; break
                }
            }

            cursorDelegate: Rectangle {
                width: 9
                color: Colors.accent
                SequentialAnimation on opacity {
                    running: filterInput.activeFocus
                    loops: Animation.Infinite
                    NumberAnimation { to: 0; duration: 450 }
                    NumberAnimation { to: 1; duration: 450 }
                }
            }
        }

        Text {
            anchors { left: filterInput.left; verticalCenter: parent.verticalCenter }
            visible: filterInput.text === ""
            text: "Filter hotkeys"
            color: Qt.alpha(Colors.textMuted, 0.7)
            font.family: Theme.fontFamily
            font.pixelSize: 14
        }

        Text {
            id: counter
            anchors { right: parent.right; rightMargin: 4; verticalCenter: parent.verticalCenter }
            text: win.filtered.length + "/" + win.entries.length
            color: Colors.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 11
        }
    }

    Rectangle {
        id: divider
        anchors { left: parent.left; right: parent.right; top: search.bottom; topMargin: win.pad }
        height: 1
        color: Qt.alpha(Colors.accent, 0.18)
    }

    Text {
        anchors { left: parent.left; right: parent.right; top: divider.bottom; margins: win.pad }
        visible: !win.hasResults
        text: win.entries.length === 0 ? "No hotkeys" : "No match"
        color: Colors.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: 13
    }

    Flickable {
        id: flick
        anchors { left: parent.left; right: parent.right; top: divider.bottom }
        height: win.hasResults ? win.listHeight : 0
        visible: win.hasResults
        contentWidth: width
        contentHeight: listColumn.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: listColumn
            width: flick.width

            Repeater {
                model: win.groups

                Column {
                    id: groupCol
                    required property var modelData
                    width: listColumn.width

                    // Same header as a launcher card: spaced capitals over a
                    // hairline, bottom-aligned so the extra height is the gap
                    // above the group.
                    Item {
                        width: groupCol.width
                        height: win.headerHeight

                        Text {
                            anchors { left: parent.left; leftMargin: win.pad; bottom: headerLine.top; bottomMargin: 5 }
                            text: groupCol.modelData.plugin.toUpperCase()
                            color: Qt.alpha(Colors.accent, 0.55)
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            font.letterSpacing: 2
                        }

                        Rectangle {
                            id: headerLine
                            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; leftMargin: win.pad; rightMargin: win.pad }
                            height: 1
                            color: Qt.alpha(Colors.accent, 0.18)
                        }
                    }

                    Repeater {
                        model: groupCol.modelData.entries

                        Item {
                            id: hkRow
                            required property var modelData
                            width: groupCol.width
                            height: win.rowHeight

                            Text {
                                id: seqText
                                anchors { left: parent.left; leftMargin: win.pad; verticalCenter: parent.verticalCenter }
                                width: win.chordColumnWidth
                                text: hkRow.modelData.sequence
                                elide: Text.ElideRight
                                color: Colors.accent
                                font.family: Theme.fontFamily
                                font.pixelSize: 13
                            }

                            Text {
                                anchors {
                                    left: seqText.right; leftMargin: 12
                                    right: unavailable.left; rightMargin: 12
                                    verticalCenter: parent.verticalCenter
                                }
                                text: hkRow.modelData.description || "—"
                                elide: Text.ElideRight
                                color: hkRow.modelData.description ? Colors.text : Colors.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: 13
                            }

                            Text {
                                id: unavailable
                                anchors { right: parent.right; rightMargin: win.pad; verticalCenter: parent.verticalCenter }
                                visible: !hkRow.modelData.registered
                                text: "unavailable"
                                color: Colors.error
                                font.family: Theme.fontFamily
                                font.pixelSize: 10
                            }
                        }
                    }
                }
            }
        }
    }

    Item {
        id: footer
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: win.footerHeight

        Text {
            anchors { left: parent.left; leftMargin: win.pad + 4; verticalCenter: parent.verticalCenter }
            text: "[type] filter   [up/down] scroll   [esc] close"
            color: Qt.alpha(Colors.textMuted, 0.8)
            font.family: Theme.fontFamily
            font.pixelSize: 10
        }
    }
}
