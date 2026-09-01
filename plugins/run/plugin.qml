import QtQuick
import QtQuick.Window
import Qwin
import "../shared"

// App launcher, PowerToys Run shaped: a hotkey summons a centered search box
// over everything, typing filters the installed apps, Enter starts the
// selected one. The list comes from the `Apps` singleton, which enumerates
// the shell's own "All apps" - so Store apps, Start-menu programs and
// (optionally) desktop shortcuts all appear, ranked by match quality and by
// how often and recently they have been launched from here.
//
// Sibling of `launcher`, which does the same for a config-defined list of
// links. This one searches the machine instead.
//
// config.json section (all keys optional):
//   "run": {
//       "hotkey": "Alt+Space",   // chord that summons it
//       "maxResults": 8,         // rows shown at once
//       "width": 620,            // frame width in pixels
//       "top": 0.3,              // search box position, as a share of screen height
//       "includeDesktop": true,  // also index Desktop shortcuts
//       "showSubtitle": true     // second line under each name (app id or path)
//   }
Window {
    id: win

    visible: false // the host leaves a deliberately hidden root hidden
    flags: Qt.FramelessWindowHint | Qt.Tool | Qt.WindowStaysOnTopHint
    color: "transparent"

    readonly property var cfg: Plugins.config("run")
    readonly property string hotkeySequence: cfg.hotkey || "Alt+Space"
    readonly property int maxResults: cfg.maxResults || 8
    readonly property int frameWidth: cfg.width || 620
    readonly property bool includeDesktop: cfg.includeDesktop !== false
    readonly property bool showSubtitle: cfg.showSubtitle !== false
    readonly property real topFraction: cfg.top || 0.3

    readonly property int pad: 14
    readonly property int rowHeight: 44
    readonly property int inputHeight: 40
    readonly property int footerHeight: 26

    // The tallest the frame can ever get: every row the config allows, plus
    // the one the run-command fallback can add.
    readonly property int maxFrameHeight:
        pad * 2 + inputHeight + 1 + (maxResults + 1) * rowHeight + footerHeight

    width: frameWidth
    height: pad * 2 + inputHeight + (rows.length > 0 ? 1 + list.height + footerHeight : 0)
    x: Screen.virtualX + (Screen.width - width) / 2

    // Pinned by its top edge, so the search box holds still while results
    // appear and disappear beneath it - the window grows downward only.
    // Keeping it on screen is clamped against maxFrameHeight rather than the
    // live height, because clamping against a height that changes per
    // keystroke would reintroduce exactly the jumping this avoids.
    y: Screen.virtualY + Math.min(Math.round(Screen.height * topFraction),
                                  Math.max(0, Screen.height - maxFrameHeight))

    // ---- data ---------------------------------------------------------------

    readonly property string query: input.text.trim()

    // Apps.search() is a call, not a bindable property, so the result is
    // pushed into `results` from the places that can change it.
    property var results: []

    // Reads the input directly rather than through the `query` binding: this
    // runs from onTextChanged, and whether a binding on the same property has
    // re-evaluated by then is not ordered. Going through `query` searched the
    // previous keystroke's text.
    function refine() {
        results = Apps.search(input.text.trim(), maxResults)
        selected = 0
    }

    // The escape hatch for a query nothing matched, and for anything that is
    // plainly meant as a command: a path, a switch, an argument. Kept last so
    // it can never take the top row away from a real app.
    readonly property bool commandLike:
        query !== "" && (results.length === 0 || /[\\\/: ]/.test(query))

    readonly property var rows: commandLike
        ? results.concat([{ id: "", name: query, subtitle: "Run command", kind: "run" }])
        : results

    property int selected: 0

    readonly property var selectedRow: selected < rows.length ? rows[selected] : null

    // ---- actions ------------------------------------------------------------

    // Hiding normally hands the keyboard back to whatever had it. Launching
    // must not: the app being started is about to ask for the foreground, and
    // handing it to the previous window first fights that.
    property bool handBackFocus: true

    function activate(row) {
        if (!row)
            return
        handBackFocus = false
        win.hide()
        if (row.kind === "run")
            Apps.runCommand(row.name)
        else
            Apps.launch(row.id)
    }

    function move(delta) {
        if (rows.length === 0)
            return
        selected = (selected + delta + rows.length) % rows.length
    }

    // ---- summoning ----------------------------------------------------------

    Hotkey {
        sequence: win.hotkeySequence
        onActivated: win.visible ? win.hide() : win.summon()
    }

    function summon() {
        // Cheap: the singleton keeps its own staleness window and only
        // re-enumerates when the list has had time to change.
        Apps.refresh()
        input.text = ""
        refine()
        System.rememberFocus()
        show()
        raise()
        requestActivate()
        input.forceActiveFocus()
    }

    onActiveChanged: if (!active && visible) hide()

    // Alt+F4 dismisses like Esc, rather than destroying the window the hotkey
    // re-summons.
    onClosing: (close) => { close.accepted = false; win.hide() }

    onVisibleChanged: {
        if (visible) {
            fadeIn.restart()
        } else if (handBackFocus) {
            System.restoreFocus()
        } else {
            handBackFocus = true
        }
    }

    NumberAnimation { id: fadeIn; target: win; property: "opacity"; from: 0; to: 1; duration: 110 }

    // The first scan costs about as much as a frame, so it happens shortly
    // after startup rather than in the first summon - by the time the hotkey
    // is pressed the list is already there.
    Timer {
        interval: 1500
        running: true
        onTriggered: Apps.refresh()
    }

    Component.onCompleted: Apps.setIncludeDesktop(win.includeDesktop)

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
            id: input
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

            onTextChanged: win.refine()

            cursorDelegate: Rectangle {
                width: 9
                color: Colors.accent
                SequentialAnimation on opacity {
                    running: input.activeFocus
                    loops: Animation.Infinite
                    NumberAnimation { to: 0; duration: 450 }
                    NumberAnimation { to: 1; duration: 450 }
                }
            }

            Keys.onUpPressed: win.move(-1)
            Keys.onDownPressed: win.move(1)
            Keys.onTabPressed: win.move(1)
            Keys.onBacktabPressed: win.move(-1)
            Keys.onReturnPressed: win.activate(win.selectedRow)
            Keys.onEnterPressed: win.activate(win.selectedRow)
            Keys.onEscapePressed: win.hide()
        }

        Text {
            anchors { left: input.left; verticalCenter: parent.verticalCenter }
            visible: input.text === ""
            text: Apps.available ? "Search apps" : "App list unavailable"
            color: Qt.alpha(Colors.textMuted, 0.7)
            font.family: Theme.fontFamily
            font.pixelSize: 14
        }

        Text {
            id: counter
            anchors { right: parent.right; rightMargin: 4; verticalCenter: parent.verticalCenter }
            text: win.results.length > 0 ? win.results.length + "/" + Apps.count : ""
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
        visible: win.rows.length > 0
    }

    Column {
        id: list
        anchors { left: parent.left; right: parent.right; top: divider.bottom }

        Repeater {
            model: win.rows

            Rectangle {
                id: row
                required property var modelData
                required property int index
                readonly property bool isSelected: win.selected === row.index

                width: list.width
                height: win.rowHeight
                color: isSelected ? Qt.alpha(Colors.accent, 0.16)
                     : hover.hovered ? Qt.alpha(Colors.surface, 0.18) : "transparent"

                // Selection marker in the gutter: a bar, not a border, so the
                // rows stay flush against each other.
                Rectangle {
                    anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
                    width: 3
                    color: row.isSelected ? Colors.accent : "transparent"
                }

                Image {
                    id: icon
                    anchors { left: parent.left; leftMargin: win.pad + 4; verticalCenter: parent.verticalCenter }
                    width: 26
                    height: 26
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    // Resolved on demand and cached in C++, so only the rows
                    // actually shown ever cost a shell lookup.
                    source: row.modelData.kind === "run" ? "" : Apps.iconFor(row.modelData.id)
                    visible: status === Image.Ready
                }

                // Stands in for the icon of a command, which has none. An app
                // whose icon simply failed to resolve gets blank space
                // instead - a prompt glyph would read as the wrong kind.
                Text {
                    anchors.centerIn: icon
                    visible: !icon.visible
                    text: row.modelData.kind === "run" ? ">_" : ""
                    color: Colors.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                }

                Column {
                    anchors {
                        left: icon.right; leftMargin: 14
                        right: kindLabel.left; rightMargin: 12
                        verticalCenter: parent.verticalCenter
                    }
                    spacing: 1

                    Text {
                        width: parent.width
                        text: row.modelData.name
                        elide: Text.ElideRight
                        color: row.isSelected ? Colors.text : Qt.alpha(Colors.text, 0.85)
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                    }

                    // Off when "showSubtitle" is false, and empty anyway for a
                    // Start-menu shortcut Windows had to synthesise an id for;
                    // the name then centers on its own.
                    Text {
                        width: parent.width
                        visible: win.showSubtitle && text !== ""
                        text: row.modelData.subtitle
                        elide: Text.ElideMiddle
                        color: Colors.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                    }
                }

                Text {
                    id: kindLabel
                    anchors { right: parent.right; rightMargin: win.pad + 4; verticalCenter: parent.verticalCenter }
                    text: row.modelData.kind === "store" ? "store"
                        : row.modelData.kind === "shortcut" ? "desktop"
                        : row.modelData.kind === "run" ? "enter" : ""
                    color: Qt.alpha(Colors.accent, 0.55)
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                }

                HoverHandler {
                    id: hover
                    onHoveredChanged: if (hovered) win.selected = row.index
                }
                TapHandler {
                    onTapped: win.activate(row.modelData)
                }
            }
        }
    }

    Item {
        id: footer
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: win.rows.length > 0 ? win.footerHeight : 0
        visible: height > 0

        Text {
            anchors { left: parent.left; leftMargin: win.pad + 4; verticalCenter: parent.verticalCenter }
            text: "[up/down] move   [enter] launch   [esc] close"
            color: Qt.alpha(Colors.textMuted, 0.8)
            font.family: Theme.fontFamily
            font.pixelSize: 10
        }
    }
}
