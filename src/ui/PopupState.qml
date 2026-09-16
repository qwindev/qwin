pragma Singleton
import QtQuick

// Which popup is open, application-wide: Popup.qml claims it on open and
// releases it on close, so opening any popup closes the incumbent without
// the plugins knowing about each other.
//
// This used to fall out of window activation, but that is a platform
// behaviour rather than a rule - it needs every popup to steal activation
// and the panel never to take it, and it changed quietly with the Qt.Tool ->
// Qt.Popup switch. One explicit property survives such changes.
//
// Per-engine, like every QML singleton: an engine rebuild starts it empty,
// while a single plugin reload leaves it alone - the lifetime we want.
QtObject {
    id: state

    // The open Popup, or null. Bindable: `PopupState.current === myPopup`.
    property var current: null

    function claim(popup) {
        if (current === popup)
            return
        // A popup destroyed by a hot reload while open reads as falsy, so
        // this skips the dismiss and drops the stale reference at once.
        if (current)
            current.dismiss()
        current = popup
    }

    function release(popup) {
        // Identity check, not a plain clear: the outgoing popup releases
        // ~100 ms after the incoming one claimed, and must not null it out.
        if (current === popup)
            current = null
    }
}
