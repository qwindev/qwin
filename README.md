# Qwin

**Beautiful desktop plugins for Windows, written in QML — no compiler, no
build step, live reload on save.**

Qwin runs desktop plugins you write in QML, on a native Qt 6 host inspired
by Quickshell on Linux. Drop a folder into the plugins directory, save a
file, and your change is on screen about a second later.

- **Frameless desktop plugins** from plain `.qml` files, draggable by
  default, hot-reloaded ~1 s after save.
- **Taskbar-style panels** that dock to a screen edge and reserve their
  space, exactly like the taskbar.
- **A real plugin API**: system stats, WiFi, Bluetooth, audio, media
  controls, the focused window, virtual desktops, global hotkeys, power
  actions, installed-apps search — and a dwindle tiling window manager.
- **Live theming**: one `colors.json` recolors every plugin without a
  reload. Everything degrades gracefully on hardware you don't have.

## A plugin

A plugin is a folder with a `manifest.json` and a `plugin.qml`:

```qml
// cpu/plugin.qml — manifest.json: { "name": "cpu", "version": "1.0.0" }
import QtQuick
import QtQuick.Window
import Qwin

Window {
    width: 220; height: 80
    visible: true
    flags: Qt.FramelessWindowHint | Qt.Tool
    color: "transparent"

    Rectangle {
        anchors.fill: parent
        radius: 10
        color: Colors.background
        Text {
            anchors.centerIn: parent
            color: Colors.text
            text: "CPU: " + System.cpuUsage.toFixed(1) + "%"
        }
    }
}
```

Save the file and it reloads in place. Bundled plugins that make this a
usable desktop out of the box live in [`plugins/`](plugins/).

## Docs

[API reference](docs/api.md) ·
[Configuration & theming](docs/configuration.md) ·
[Building](docs/building.md) ·
[Packaging](docs/packaging.md)

## License

[MIT](LICENSE). Qt is used under LGPL-3.0 (dynamically linked);
[VirtualDesktopAccessor.dll](https://github.com/Ciantic/VirtualDesktopAccessor)
(MIT) is bundled in `third_party/` and loaded at runtime.
