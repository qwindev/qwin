import QtQuick
import qwin 1.0
import "../shared"

// The focused window's title, app icon and optional muted app-name prefix.
// Hides itself through the usual `shown` whenever nothing focusable owns the
// keyboard - the desktop, a system flyout, one of our own panels. No
// MouseArea: a click here would have nothing to do.
//
// config.json section (all keys optional):
//   "window-title": {
//       "maxWidth": 320,    // title width before it elides
//       "showIcon": true,
//       "showApp": false    // prefix the app name, "chrome  -  Page title"
//   }
Rectangle {
    id: windowTitle
    property bool shown: ActiveWindow.available
    visible: shown

    // Read once per load; a config.json edit triggers a full reload anyway.
    readonly property var cfg: Plugins.config("window-title")
    readonly property int maxWidth: cfg.maxWidth || 320
    // Explicit undefined checks, not `||`: "showIcon": false must survive.
    readonly property bool showIcon: cfg.showIcon === undefined ? true : cfg.showIcon
    readonly property bool showApp: cfg.showApp === undefined ? false : cfg.showApp

    // Apps can leave a gap between window creation and their first
    // WM_SETTEXT; appName carries the bar through it.
    readonly property string displayTitle: ActiveWindow.title.length > 0 ? ActiveWindow.title
                                                                          : ActiveWindow.appName

    // "Which window", not "what its title says", so the fade below runs on a
    // focus change and not on every keystroke in an address bar.
    readonly property string identityKey: ActiveWindow.available + "|" + ActiveWindow.processName
                                          + "|" + ActiveWindow.appName + "|" + ActiveWindow.iconSource

    height: 24
    radius: 5
    color: "transparent"
    width: row.implicitWidth + 14

    // A hard content swap on every Alt+Tab reads as a flicker. restart()
    // always re-runs from the first step, so even a burst of Alt+Tabs cannot
    // park this at partial opacity - the last restart finishes it.
    SequentialAnimation {
        id: fade
        NumberAnimation { target: windowTitle; property: "opacity"; to: 0.35; duration: 60; easing.type: Easing.OutQuad }
        NumberAnimation { target: windowTitle; property: "opacity"; to: 1.0; duration: 160; easing.type: Easing.OutQuad }
    }
    onIdentityKeyChanged: fade.restart()

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6

        Image {
            anchors.verticalCenter: parent.verticalCenter
            visible: windowTitle.showIcon && ActiveWindow.iconSource.length > 0
            source: ActiveWindow.iconSource
            width: 16
            height: 16
            fillMode: Image.PreserveAspectFit
            smooth: true
            mipmap: true
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: windowTitle.showApp && ActiveWindow.appName.length > 0
            text: ActiveWindow.appName + "  -  "
            color: Colors.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: windowTitle.displayTitle
            color: Colors.text
            font.family: Theme.fontFamily
            font.pixelSize: 13
            elide: Text.ElideRight
            width: Math.min(implicitWidth, windowTitle.maxWidth)
        }
    }
}
