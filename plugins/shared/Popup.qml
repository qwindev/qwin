import QtQuick
import QtQuick.Window
import qwin 1.0
import "."

// Anchored popup for panel plugins. A panel is too thin to render one
// inside, so this is a separate frameless always-on-top window that unfolds
// below `anchorItem`. Use open()/dismiss()/toggle(); the inherited close()
// works too but skips the fade-out. A click outside or Escape dismisses.
// Single-level by design - see PopupState.qml.
Window {
    id: popup

    property Item anchorItem
    property int contentWidth: 320
    property int contentHeight: 200
    property int gap: 8 // below the anchor's bottom edge
    property color backgroundColor: "#F0101418"
    property color borderColor: "#3355D6C2"
    default property alias content: contentSlot.data

    readonly property bool opened: visible

    // Qt.Popup, not Qt.Tool - but neither hands the keyboard back here: on
    // hide the foreground goes to the bar that owns the popup, which has
    // nothing to type into (measured under both). Hotkey-driven popups, whose
    // owner is an ordinary window, DO get it back, which is what once made
    // the flag look like a replacement for the focus bracket. It is not.
    flags: Qt.FramelessWindowHint | Qt.Popup | Qt.WindowStaysOnTopHint
    color: "transparent"
    width: contentWidth
    height: contentHeight
    visible: false

    // The fade MUST run on the window's alpha, never an item's. Showing a
    // window presents its last rendered surface until a new frame arrives,
    // and for a popup that was up moments ago that is the fully drawn popup:
    // an item fade flashed it at full opacity, blinked out ~25 ms later when
    // the first real frame landed, and only then faded. The compositor
    // applies window alpha to whatever is presented, stale frame included.
    opacity: 0

    // Reopen guard: clicking the anchor while open closes the popup through
    // focus loss first, and the click's own toggle() would then reopen it.
    // Every close path lands here, the inherited close() included.
    property double _closedAt: 0
    onVisibleChanged: if (!visible) {
        _closedAt = Date.now()
        PopupState.release(popup)
        System.restoreFocus()
    }

    function open() {
        if (visible || !anchorItem || Date.now() - _closedAt < 150)
            return
        // Set the screen first: a never-shown Window reports the primary
        // one, mis-clamping a popup whose panel is on another monitor.
        const anchorWin = anchorItem.Window.window
        if (anchorWin && anchorWin.screen)
            popup.screen = anchorWin.screen
        const p = anchorItem.mapToGlobal(0, anchorItem.height)
        let nx = Math.round(p.x + anchorItem.width / 2 - width / 2)
        nx = Math.max(popup.screen.virtualX + 8,
                      Math.min(nx, popup.screen.virtualX + popup.screen.width - width - 8))
        x = nx
        y = Math.round(p.y + gap)
        closeAnim.stop()
        // Map transparent: `from: 0` only applies at the animation's first
        // tick, 20-40 ms after the window is up, and a popup Qt closed
        // itself (a click outside bypasses dismiss()) left the alpha at 1.
        popup.opacity = 0
        // Activating steals the keyboard, so remember what had it. Only the
        // first popup of a chain does: switching straight to another module
        // dismisses this one, whose restore bails without consuming the saved
        // window, so the app the user came from survives the switch.
        if (!PopupState.current)
            System.rememberFocus()
        // Before showing, so the outgoing fade overlaps this unfold.
        PopupState.claim(popup)
        visible = true
        popup.requestActivate()
        openAnim.restart()
    }

    function dismiss() {
        if (!visible || closeAnim.running)
            return
        openAnim.stop()
        closeAnim.restart()
    }

    function toggle() {
        if (visible)
            dismiss()
        else
            open()
    }

    onActiveChanged: if (!active) dismiss()

    Shortcut {
        sequence: "Escape"
        onActivated: popup.dismiss()
    }

    Item {
        id: animRoot
        width: popup.width
        height: popup.height

        Rectangle {
            anchors.fill: parent
            radius: 12
            color: popup.backgroundColor
            border.color: popup.borderColor
            border.width: 1
        }

        Item {
            id: contentSlot
            anchors.fill: parent
            anchors.margins: 12
        }
    }

    ParallelAnimation {
        id: openAnim
        NumberAnimation { target: animRoot; property: "y"; from: -8; to: 0; duration: 150; easing.type: Easing.OutCubic }
        NumberAnimation { target: popup; property: "opacity"; from: 0; to: 1; duration: 150; easing.type: Easing.OutCubic }
    }

    SequentialAnimation {
        id: closeAnim
        NumberAnimation { target: popup; property: "opacity"; to: 0; duration: 100; easing.type: Easing.InCubic }
        ScriptAction { script: popup.visible = false }
    }
}
