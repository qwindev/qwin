pragma Singleton
import QtQuick
import Qwin

// App-wide non-color theming, from config.json's "theme" section (the name
// is reserved in the registry, like "enabled"). Read once per load: a
// config.json edit rebuilds the engine, so this is live without watching
// anything itself.
QtObject {
    readonly property var _cfg: Plugins.config("theme")
    readonly property string fontFamily: _cfg.fontFamily || "Cascadia Code"
}
