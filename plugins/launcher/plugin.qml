import QtQuick
import QtQuick.Window
import Qwin
import "../shared"

// Link launcher: a hotkey summons a centered overlay of the link cards its
// config.json section defines ("groups": [{name, links: [{name, url}]}]),
// with the chord itself configurable via a "hotkey" key in that same
// "launcher" section (default Alt+Shift+B). Type to filter, arrow keys to
// move, Enter or a click opens in the default browser and hides; Esc or
// focus loss just hides. A config.json edit applies immediately - it
// hot-reloads the plugin, and the section is re-read on every summon
// besides.
Window {
    id: win

    visible: false // the host leaves a deliberately hidden root hidden
    flags: Qt.FramelessWindowHint | Qt.Tool | Qt.WindowStaysOnTopHint
    color: "transparent"

    readonly property var cfg: Plugins.config("launcher")
    readonly property string hotkeySequence: cfg.hotkey || "Alt+Shift+B"

    readonly property int pad: 24
    readonly property int cardWidth: 250

    // Captured from the unfiltered layout and frozen while a filter is
    // typed: the window must not resize under the user's keystrokes. It
    // re-fits only while the filter is empty; see the handlers below.
    property real frameWidth: 430
    property real frameHeight: 160
    readonly property real panelWidth: frameWidth

    width: frameWidth + 2 * pad
    height: frameHeight + content.spacing + footer.height + 2 * pad
    x: Screen.virtualX + (Screen.width - width) / 2
    y: Screen.virtualY + (Screen.height - height) * 0.38

    // ---- data -------------------------------------------------------------

    property var groups: []      // [{name, links: [{name, url}]}]

    // The registry keeps config.json parsed, and last-good on bad JSON, so
    // there is nothing to error-handle here.
    function reload() {
        groups = Plugins.config("launcher").groups || []
    }

    readonly property string query: filterInput.text.trim().toLowerCase()

    // Anchored to word starts rather than matching anywhere in the string, so
    // that one letter already narrows: a substring test let any letter of
    // "overflow" or "ycombinator" pull in a link nobody was looking for.
    function matches(text, q) {
        const s = (text || "").toLowerCase()
        if (s.startsWith(q))
            return true
        for (let i = 1; i < s.length; ++i)
            if (!/[a-z0-9]/.test(s[i - 1]) && s.startsWith(q, i))
                return true
        return false
    }

    // The parts every url shares - scheme, "www.", the TLD - carry no signal
    // and are dropped before matching: with them in, "s" and "c" alone matched
    // all sixteen bookmarks through "https" and ".com".
    function searchableUrl(url) {
        const s = (url || "").toLowerCase().replace(/^[a-z]+:\/\/(www\.)?/, "")
        const cut = s.indexOf("/")
        const host = cut < 0 ? s : s.slice(0, cut)
        return host.replace(/\.[a-z]{2,}$/, "") + (cut < 0 ? "" : s.slice(cut))
    }

    // A link matches on its own name/url or its group's name; groups left
    // empty disappear.
    readonly property var visibleGroups: {
        const out = []
        for (const g of groups) {
            const name = g.name || "?"
            const links = (g.links || []).filter(l =>
                query === ""
                || win.matches(name, query)
                || win.matches(l.name, query)
                || win.matches(win.searchableUrl(l.url), query))
            if (links.length > 0)
                out.push({ name: name, links: links })
        }
        return out
    }

    readonly property int totalLinks: groups.reduce((n, g) => n + (g.links ? g.links.length : 0), 0)
    readonly property int visibleLinks: visibleGroups.reduce((n, g) => n + g.links.length, 0)

    // Roughly square in card count, capped by screen width. Off the full
    // set, so the cards keep their column rhythm while a filter is typed.
    readonly property int gridColumns: {
        const n = groups.length
        if (n === 0)
            return 1
        const fit = Math.max(1, Math.floor(Screen.width * 0.85 / (cardWidth + grid.columnSpacing)))
        return Math.min(Math.ceil(Math.sqrt(n)), fit, n)
    }

    // ---- selection & navigation -------------------------------------------

    property int selGroup: 0
    property int selLink: 0

    onVisibleGroupsChanged: { selGroup = 0; selLink = 0 }

    readonly property var selectedLink: {
        if (selGroup < visibleGroups.length) {
            const g = visibleGroups[selGroup]
            if (selLink < g.links.length)
                return g.links[selLink]
        }
        return null
    }

    // Up/Down walk a card's links and continue into the card above/below in
    // the same column, wrapping within it; Left/Right jump to the
    // neighbouring card, keeping the position in the list.
    function moveVertical(dir) {
        const n = visibleGroups.length
        if (n === 0)
            return
        let g = selGroup
        const l = selLink + dir
        if (l >= 0 && l < visibleGroups[g].links.length) {
            selLink = l
            return
        }
        const cols = gridColumns
        const col = g % cols
        g += dir * cols
        if (g < 0 || g >= n) {
            g = col                 // wrap: top card of this column...
            if (dir < 0)
                while (g + cols < n) // ...or walk down to its bottom card
                    g += cols
        }
        selGroup = g
        selLink = dir > 0 ? 0 : visibleGroups[g].links.length - 1
    }

    function moveHorizontal(dir) {
        const n = visibleGroups.length
        if (n === 0)
            return
        selGroup = (selGroup + dir + n) % n
        selLink = Math.min(selLink, visibleGroups[selGroup].links.length - 1)
    }

    function openSelected() {
        if (selectedLink)
            openLink(selectedLink.url)
    }

    function openLink(url) {
        win.hide()
        Qt.openUrlExternally(url)
    }

    // ---- summoning ----------------------------------------------------------

    Hotkey {
        sequence: win.hotkeySequence
        onActivated: win.visible ? win.hide() : win.summon()
    }

    function summon() {
        reload()
        filterInput.text = ""
        selGroup = 0
        selLink = 0
        // Settle now, so the frame capture happens before the window shows -
        // positioners otherwise re-layout a frame later.
        grid.forceLayout()
        content.forceLayout()
        // Summoning takes the keyboard; hiding gives it back.
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
    NumberAnimation { id: fadeIn; target: win; property: "opacity"; from: 0; to: 1; duration: 120 }

    // ---- ui -----------------------------------------------------------------

    Rectangle {
        anchors.fill: parent
        radius: 12
        color: Qt.alpha(Colors.background, 0.96)
        border.color: Qt.alpha(Colors.accent, 0.35)
        border.width: 1
    }

    Column {
        id: content
        x: win.pad
        y: win.pad
        spacing: 16

        onImplicitHeightChanged: if (win.query === "") win.frameHeight = implicitHeight

        Item {
            width: win.panelWidth
            height: 40

            TextInput {
                id: filterInput
                anchors {
                    left: parent.left
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

                Keys.onUpPressed: win.moveVertical(-1)
                Keys.onDownPressed: win.moveVertical(1)
                Keys.onLeftPressed: win.moveHorizontal(-1)
                Keys.onRightPressed: win.moveHorizontal(1)
                Keys.onTabPressed: win.moveHorizontal(1)
                Keys.onReturnPressed: win.openSelected()
                Keys.onEnterPressed: win.openSelected()
                Keys.onEscapePressed: win.hide()
            }

            Text {
                anchors { left: filterInput.left; verticalCenter: parent.verticalCenter }
                visible: filterInput.text === ""
                text: "Search bookmarks"
                color: Qt.alpha(Colors.textMuted, 0.7)
                font.family: Theme.fontFamily
                font.pixelSize: 14
            }

            Text {
                id: counter
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: win.visibleLinks + "/" + win.totalLinks
                color: Colors.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 11
            }
        }

        Rectangle { width: win.panelWidth; height: 1; color: Qt.alpha(Colors.accent, 0.18) }

        Text {
            visible: win.visibleGroups.length === 0
            width: win.panelWidth
            wrapMode: Text.WrapAnywhere
            text: win.groups.length === 0
                ? "// no links defined in config.json (\"launcher\" section)"
                : "// no matches"
            color: Colors.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }

        Grid {
            id: grid
            columns: win.gridColumns
            columnSpacing: 16
            rowSpacing: 16
            verticalItemAlignment: Grid.AlignTop

            onImplicitWidthChanged: if (win.query === "") win.frameWidth = Math.max(implicitWidth, 430)

            Repeater {
                model: win.visibleGroups

                Column {
                    id: card
                    required property var modelData
                    required property int index
                    width: win.cardWidth
                    spacing: 5

                    Text {
                        text: card.modelData.name.toUpperCase()
                        color: Colors.accent
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        font.letterSpacing: 2
                    }

                    Rectangle { width: parent.width; height: 1; color: Qt.alpha(Colors.accent, 0.18) }

                    Repeater {
                        model: card.modelData.links

                        Rectangle {
                            id: linkRow
                            required property var modelData
                            required property int index
                            readonly property bool selected: win.selGroup === card.index
                                                             && win.selLink === linkRow.index
                            width: parent.width
                            height: 28
                            radius: 4
                            color: selected ? Qt.alpha(Colors.accent, 0.16)
                                 : hover.hovered ? Qt.alpha(Colors.surface, 0.18) : "transparent"
                            border.color: selected ? Qt.alpha(Colors.accent, 0.5) : "transparent"
                            border.width: 1

                            Text {
                                anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
                                text: linkRow.modelData.name || linkRow.modelData.url
                                color: linkRow.selected ? Colors.text : Qt.alpha(Colors.text, 0.85)
                                font.family: Theme.fontFamily
                                font.pixelSize: 14
                            }

                            HoverHandler {
                                id: hover
                                onHoveredChanged: if (hovered) {
                                    win.selGroup = card.index
                                    win.selLink = linkRow.index
                                }
                            }
                            TapHandler {
                                onTapped: win.openLink(linkRow.modelData.url)
                            }
                        }
                    }
                }
            }
        }

    }

    // Pinned to the frame bottom rather than part of the column, so it stays
    // put while the filtered grid shrinks.
    Item {
        id: footer
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: win.pad }
        height: 14

        Text {
            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
            text: win.selectedLink ? win.selectedLink.url : ""
            elide: Text.ElideMiddle
            color: Qt.alpha(Colors.accent, 0.55)
            font.family: Theme.fontFamily
            font.pixelSize: 10
        }
    }

    Component.onCompleted: reload()
}
