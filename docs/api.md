# Plugin API reference

Everything a plugin can use comes from one import:

```qml
import Qwin
```

Singletons: [`System`](#system) · [`Plugins`](#plugins-registry--config) ·
[`Colors`](#colors-theming) · [`Desktops`](#desktops-virtual-desktops) ·
[`Tiler`](#tiler-window-tiling) · [`Wifi`](#wifi) · [`Media`](#media-now-playing) ·
[`Audio`](#audio-volume--output-devices) · [`ActiveWindow`](#activewindow-focused-window) ·
[`Power`](#power) · [`Bluetooth`](#bluetooth) · [`Apps`](#apps-installed-apps)

Types: [`Hotkey`](#hotkey-global-hotkeys) · [`PanelWindow`](#panelwindow-taskbar-style-panels)

Plus the [shared components](#shared-components) in `<plugins-dir>\shared\`.

## System

| Member | Description |
|---|---|
| `System.cpuUsage` | Total CPU usage in percent, refreshed every second (`statsChanged`). |
| `System.memoryUsagePercent` | Physical memory load in percent, refreshed every second. |
| `System.hostname` | Machine host name (constant). |
| `System.readTextFile(path)` | Returns the file content as a string. Relative paths resolve against the plugins directory; paths escaping the plugins directory (after canonicalization) are rejected and return `""`. |
| `System.openStartMenu()` | Opens the Windows Start menu, or closes it again — it toggles, like the key it synthesizes (Ctrl+Esc). |
| `System.rememberFocus()` / `System.restoreFocus()` | Focus bracket for overlays and popups: call `rememberFocus()` before taking the keyboard (showing a popup or overlay), `restoreFocus()` after hiding, so the window the user was working in gets the keyboard back. `shared/Popup.qml` and the bundled `run`/`launcher` overlays use it. |
| `System.batteryAvailable` | Whether the machine has a battery at all (`false` on desktops — the bundled `battery` plugin hides itself on that). |
| `System.batteryPercent` | Charge 0–100, or `-1` when Windows reports it as unknown. |
| `System.batteryCharging` | Whether the battery is actively charging. |
| `System.acPower` | Whether the machine is running on mains. Distinct from `batteryCharging`: plugged in and full is `acPower` without `batteryCharging`. |
| `System.batteryTimeLeft` | Estimated seconds of runtime left, or `-1` when unknown (which includes being on AC). |
| `System.batterySaver` | Whether Windows battery saver is on. |

The battery values are polled on the same one-second timer as the stats but
notify through their own `batteryChanged` signal — they change rarely, and
routing them through the per-second `statsChanged` would re-run every
unrelated binding once a second.

## Plugins (registry & config)

| Member | Description |
|---|---|
| `Plugins.config(name)` | The named plugin's section from `config.json` as a plain object (`{}` when absent). Plugins read their own section: `Plugins.config("clock").format`. |
| `Plugins.source(name)` | URL of the named plugin's `plugin.qml`, for embedding it with a `Loader`; empty (plus a log warning) for unknown names. |
| `Plugins.has(name)` | Whether a plugin with that manifest name is installed. |

A container plugin embeds others by listing their names in its own
`config.json` section and instantiating each with
`Loader { source: Plugins.source(name) }` — this is how the bundled `bar`
assembles its slots. Plugins meant as modules of another plugin keep an
`Item` root and are simply left out of `enabled`.

A module that wants to hide itself (e.g. a WiFi module on a machine with no
adapter) declares `property bool shown` on its root, and the container binds
its Loader slot's `visible` to that. Do not bind on the loaded item's
`visible` for this: it reports effective visibility (parent chain included),
which feeds back through the Loader and deadlocks invisible.

`Plugins.config()` values are plain snapshots, not live bindings — editing
`config.json` reloads the affected plugins instead, so reading the config
once at load is always in sync. Changing a plugin that others embed reloads
the embedders too (via a full engine rebuild, same as `shared/` edits).

## Colors (theming)

A color palette shared by all plugins, backed by `<plugins-dir>\colors.json`.
The file is created with a default palette on first run; every key in it
becomes a color property:

```qml
Rectangle {
    color: Colors.background
    border.color: Colors.accent
    Text { color: Colors.text; text: "themed" }
}
```

Built-in roles (always present; the file overrides their values):
`background`, `surface`, `text`, `textMuted`, `accent`, `warning`, `error`.
Any additional key you add to the file becomes a property too
(`"clockHands": "#FF0000"` → `Colors.clockHands`).

- **The file is watched:** saving `colors.json` recolors every bound plugin
  live (within ~0.5 s) — plugins are *not* reloaded, bindings just update.
- Values are anything QML accepts as a color: `#RRGGBB`, `#AARRGGBB`, or
  named colors. Keys must be plain identifiers starting with a lowercase
  letter (anything else is unreachable from QML and gets ignored).
- Bad input never breaks plugins: invalid JSON keeps the current palette, an
  invalid color value keeps that key's current color (with a log warning
  either way). Deleting a key from the file resets a built-in role to its
  default.
- The palette is one-way: assigning to `Colors.*` from QML is rejected with a
  warning — edit the file instead.
- A brand-new key added while the app runs does not wake bindings written
  before the key existed; the plugin's next (hot) reload picks it up.
  Changing *values* of existing keys always applies live.

`Colors` is colors only. Non-color theming — currently the font family the
bundled plugins use — lives in config.json's `"theme"` section, served by
the `Theme` singleton in `shared/` (see
[shared components](#shared-components)).

## Hotkey (global hotkeys)

Registers a **system-wide** keyboard shortcut via the Win32 `RegisterHotKey`
API — it fires no matter which application has focus, unlike QtQuick's
`Shortcut`, which only works while the plugin's own window is focused:

```qml
Hotkey {
    sequence: "Shift+Alt+1"        // modifiers + one key, Qt shortcut syntax
    onActivated: Desktops.switchTo(0)
}
```

| Member | Description |
|---|---|
| `sequence` | The chord, e.g. `"Shift+Alt+1"`, `"Ctrl+F9"`, `"Meta+Space"`. Modifiers: `Ctrl`, `Alt`, `Shift`, `Meta` (the Windows key); one main key (letter, digit, F1–F24, arrows, and other common keys). |
| `enabled` | Set to `false` to release the chord without removing the declaration (default `true`). |
| `registered` | Read-only: whether the OS registration succeeded. |
| `activated()` | Emitted on every press of the chord (once per press, no auto-repeat). |

- The chord is grabbed **exclusively** while registered: the focused
  application never sees it. Pick combinations that don't collide with keys
  you type.
- If another application (or another plugin) already owns the chord,
  registration fails with a log warning and `registered` stays `false` — the
  plugin itself keeps working.
- The registration is released automatically when the plugin is removed,
  hot-reloaded, or the app quits.

## PanelWindow (taskbar-style panels)

Use `PanelWindow` as the plugin root to dock a plugin to a screen edge and
**reserve its space** — maximized applications stop at its border, exactly
like the taskbar (Quickshell's PanelWindow, implemented with the Windows
AppBar API):

```qml
import QtQuick
import Qwin

PanelWindow {
    edge: Qt.TopEdge   // Qt.TopEdge | Qt.BottomEdge | Qt.LeftEdge | Qt.RightEdge
    thickness: 36      // depth of the bar in logical pixels
    visible: true
    color: "#F0101418"

    Text { anchors.centerIn: parent; color: "white"; text: "hello bar" }
}
```

- The panel spans the whole screen along its edge; `width`/`height` set in
  QML are ignored — `edge` and `thickness` control the geometry.
- Multiple panels stack: a second `Qt.TopEdge` panel is placed below the
  first, each reserving its own strip.
- The reservation is released automatically when the plugin is removed,
  hot-reloaded, or the app quits.

## Desktops (virtual desktops)

| Member | Description |
|---|---|
| `Desktops.available` | `VirtualDesktopAccessor.dll` loaded next to the exe (constant). When `false`, `count` stays 1 and the calls are warning no-ops. |
| `Desktops.count` | Number of virtual desktops. |
| `Desktops.currentIndex` | Index of the active desktop (0-based). |
| `Desktops.switchTo(index)` | Switch to the given desktop. |
| `Desktops.createDesktop()` | Create a new desktop and switch to it. |
| `Desktops.closeCurrentDesktop()` | Close the active desktop (refuses on the last one). |
| `Desktops.moveForegroundWindowToNewDesktop()` | Send the focused application window to a fresh desktop, switch to it and maximize the window (skips plugin/shell windows; respects windows that forbid maximizing). Bind it to a `Hotkey` — a chord press doesn't move focus, so the foreground window is the one the user was in. |

Both properties notify through the `changed` signal, so bindings update
automatically (including when desktops are created/removed or switched
outside the plugin). Windows has no public virtual-desktop API; everything
goes through [VirtualDesktopAccessor.dll](https://github.com/Ciantic/VirtualDesktopAccessor)
(MIT, bundled in `third_party/` and copied next to the exe at build time),
which wraps the undocumented COM interfaces. Switches — including external
ones (Win+Ctrl+Arrow, Task View) — are picked up instantly via the DLL's
notification hook; desktop creation/removal is caught by a 1 s poll.

## Tiler (window tiling)

A dwindle tiling window manager over the desktop's own windows: each new
window splits the focused one along its longer side, per monitor and per
virtual desktop. Dragging an inner edge rewrites the split behind it, a
title-bar drag onto another tile swaps the two, and fixed-size dialogs are
never tiled. Windows that keep refusing their assigned rect are floated
rather than fought with.

The layout state lives in C++ and survives hot reloads — a plugin (see
`plugins/tiling`) only pushes config in and binds `Hotkey` chords to
the commands.

| Member | Description |
|---|---|
| `Tiler.enabled` | Read/write master switch. Disabling restores every window's pre-adoption geometry. |
| `Tiler.gap` / `Tiler.outerGap` | Gap between tiles / to the work-area edge, logical px. |
| `Tiler.minWidth` / `Tiler.minHeight` | Smallest tile a split may create, logical px. A window that cannot be placed without breaking these stays floating and is reclaimed once room frees up. |
| `Tiler.resizeStep` | How far one `resize()` call moves a divider, logical px. |
| `Tiler.floatProcesses` | Executable names (`"spotify.exe"`, case-insensitive) that are never tiled. |
| `Tiler.managedCount` | Number of currently tiled windows (`layoutChanged`). |
| `Tiler.debug` | Log every adoption and every rect applied. Off by default: it is one line per window per re-tile. |
| `Tiler.focusDirection(dir)` | Focus the neighbouring window: `"left"`, `"right"`, `"up"`, `"down"`. |
| `Tiler.moveDirection(dir)` | Swap the focused window with its neighbour in that direction. |
| `Tiler.resize(how)` | Move the divider nearest the focused window: `"wider"`, `"narrower"`, `"taller"`, `"shorter"`. |
| `Tiler.toggleFloating()` | Take the focused window out of the layout (restoring its adopted size), or put it back in. |
| `Tiler.toggleSplit()` | Flip the split that placed the focused window — the one-key fix for a dwindle that divided the wrong way. |
| `Tiler.equalize()` | Forget every resize on the focused window's monitor. |
| `Tiler.retile()` | Re-apply the layout now. |

## Wifi

Reports the current connection and delegates everything else to Windows. On
machines without a WLAN adapter `Wifi.available` is `false` and everything
else is inert.

| Member | Description |
|---|---|
| `Wifi.available` | A WLAN adapter exists and the WLAN service answered (constant). |
| `Wifi.connected` | Whether a connection currently exists. |
| `Wifi.detailsAvailable` | Whether `ssid` and `signalPercent` could be read. `false` when Windows Location services are off — `connected` stays accurate, but the name and signal are unavailable. |
| `Wifi.ssid` | SSID of the current connection (empty when disconnected, or when `detailsAvailable` is `false`). |
| `Wifi.signalPercent` | Signal quality 0–100 of the current connection (0 when `detailsAvailable` is `false`). |
| `Wifi.rxBytesPerSec` / `Wifi.txBytesPerSec` | Download/upload rate on the WLAN adapter, bytes per second. |
| `Wifi.openNetworkFlyout()` | Opens the native network flyout — the same available-networks popup the taskbar tray opens. |

Connection state refreshes on a slow poll (3 s); throughput samples the
adapter's byte counters every second and notifies through its own
`throughputChanged` signal, so a rate binding does not re-run every SSID
binding in the process. Throughput covers the WLAN adapter only — VPN
tunnels and ethernet are not counted — and is not permission-gated.

Windows gates the SSID behind the Location capability (Settings > Privacy &
security > Location, including "Let desktop apps access your location"). With
it off, `connected` and the throughput properties keep working while
`detailsAvailable` stays `false`.

## Media (now playing)

Reports whatever application currently owns Windows' system-wide "now
playing" state — Spotify, a browser tab, VLC — through the Global System
Media Transport Controls. There is no per-application integration and
nothing to configure. With nothing playing anywhere, `Media.available` is
`false` and the bundled `media` plugin hides itself.

| Member | Description |
|---|---|
| `Media.available` | Whether a media session exists at all. |
| `Media.title` | Current track title (empty until the app reports metadata). |
| `Media.artist` | Current track artist. |
| `Media.album` | Current album title. |
| `Media.sourceApp` | The session's app id, e.g. `Spotify.exe` — useful as a label before metadata arrives. |
| `Media.playing` | Whether playback status is *playing* (as opposed to paused or stopped). |
| `Media.canPlayPause` | Whether the session accepts a play/pause toggle. |
| `Media.canGoNext` / `Media.canGoPrevious` | Whether skip forward/back are offered. |
| `Media.playPause()` / `Media.next()` / `Media.previous()` | Transport controls; no-ops when there is no session. |

Everything is event-driven (no poll timer): the session manager pushes
current-session, metadata and playback-state changes. One `changed` signal
covers every property.

## Audio (volume & output devices)

Wraps Core Audio for the default output endpoint. With no active output
device — every endpoint unplugged, absent or disabled — `Audio.available` is
`false`, the properties stay inert, and the log says so once.

| Member | Description |
|---|---|
| `Audio.available` | Whether an active output endpoint exists. |
| `Audio.volume` | Master volume of the default output, 0–100. |
| `Audio.muted` | Endpoint mute state. |
| `Audio.deviceName` | Friendly name of the current default output. |
| `Audio.devices` | Active output endpoints, each `{ id, name, isDefault }`. |
| `Audio.canSwitchDevices` | Whether switching the default output is possible (see below). |
| `Audio.setVolume(v)` / `Audio.adjustVolume(delta)` | Set or nudge the level; `adjustVolume` works off the live value, so repeated scroll ticks accumulate exactly. |
| `Audio.toggleMute()` / `Audio.setMuted(b)` | Mute control. |
| `Audio.setDefaultDevice(id)` | Make the given endpoint the default output for all three roles (console, multimedia, communications) — leaving one behind is why "I switched but my call app stayed on the old speakers" happens. |
| `Audio.openSoundSettings()` | Opens the native Sound settings page. |

Volume and mute changes made anywhere on the system (the volume keys,
another app's mixer) arrive through Core Audio callbacks rather than a poll,
and notify through `volumeChanged`; the device list, default device and its
name notify separately through `devicesChanged`, so dragging a slider does
not re-run every device-list binding.

Windows exposes **no public API for changing the default output device**.
`setDefaultDevice` uses the undocumented `IPolicyConfig` interface — stable
since Vista and what most third-party volume mixers rely on, but not
guaranteed. If it cannot be created, `canSwitchDevices` is `false` and
plugins should fall back to `openSoundSettings()`; the bundled `volume`
plugin does exactly that.

## ActiveWindow (focused window)

Reports the window that currently has the keyboard — what makes a bar useful
in a tiling/virtual-desktop setup. It is driven by `SetWinEventHook`, not a
poll, and uses the same "is this a real application window" rule as the rest
of the host, so the desktop, system flyouts and Qwin's own panels never
appear here.

| Member | Description |
|---|---|
| `ActiveWindow.available` | Whether a real application window has focus. |
| `ActiveWindow.title` | Its title, updated live as the app rewrites it (tabs, documents). |
| `ActiveWindow.processName` | Owning executable, e.g. `chrome.exe`. Empty for elevated/protected processes, which deny the query. |
| `ActiveWindow.appName` | Friendlier label: the exe's `FileDescription` (e.g. `Google Chrome`), falling back to its file name. |
| `ActiveWindow.iconSource` | The window/app icon as a `data:image/png;base64,...` URL, ready to bind straight to an `Image`. Empty when no icon could be read. |

Icons are cached per executable, so a chatty title (a browser address bar
being typed into) never re-encodes a PNG.

## Power

The session's power state: the keep-awake toggle and the session actions.

| Member | Description |
|---|---|
| `Power.keepAwake` | Read/write. While `true`, the machine will not sleep and the display will not blank. Disarmed automatically when the host exits. |
| `Power.hibernateAvailable` | Whether hibernation is both supported and currently enabled, so a menu can omit a dead entry (constant). |
| `Power.lock()` | Locks the session. |
| `Power.sleep()` / `Power.hibernate()` | Suspends to RAM / to disk. |
| `Power.signOut()` / `Power.restart()` / `Power.shutdown()` | Ends the session, reboots, or powers off. |

The session actions are **immediate and irreversible** — this singleton does
what it is told, with no confirmation step of its own. Any confirmation
belongs in the plugin; the bundled `power` plugin requires a second click on
an armed row before it acts. None of them force applications to close, so an
app with unsaved work can still block the request, exactly as it would from
the Start menu.

## Bluetooth

Scoped like `Wifi`: it reports what is paired and what is connected, and
hands pairing, connecting and the radio toggle to Windows' own settings
page. No radio means `available == false` and the bundled plugin hides
itself.

| Member | Description |
|---|---|
| `Bluetooth.available` | Whether a Bluetooth radio is present. |
| `Bluetooth.connectedCount` | How many paired devices are connected right now. |
| `Bluetooth.devices` | Paired devices, each `{ name, connected, battery }`. |
| `Bluetooth.refresh()` | Re-enumerates immediately instead of waiting for the next poll. |

`battery` is 0–100 where Windows exposes a battery level for the device, and
`-1` otherwise — the common case: it is only populated for connected
Bluetooth LE devices that publish a battery service. State is polled every
5 seconds (there is no cheap notification API for it) and `changed` only
fires when something actually moved.

## Apps (installed apps)

The search index behind the bundled `run` plugin: the list of installed
applications, matched by name and ranked by how well the query fits and how
often the app has been launched from here.

The list comes from `shell:AppsFolder`, the shell namespace behind Start's
"All apps" — already the union of the per-user and all-users Start Menu
shortcut trees *and* packaged/Store apps, which have no shortcut on disk
anywhere. Desktop shortcuts are the one thing it does not carry, so those
are scanned separately when `setIncludeDesktop` is on — and dropped when
they merely repeat a name the shell already lists.

| Member | Description |
|---|---|
| `Apps.available` | Whether the app list could be enumerated at all. |
| `Apps.count` | Number of indexed entries. |
| `Apps.setIncludeDesktop(on)` | Also index `.lnk`/`.url` files on the user and public Desktop. Applies from the next scan, and forces one if the value changed. |
| `Apps.refresh(force)` | Rescan if the last scan is older than 5 minutes; `force` ignores the window. |
| `Apps.search(query, limit)` | Ranked matches, best first: `{ id, name, subtitle, kind }`, where `kind` is `"store"`, `"app"` or `"shortcut"`. An empty query returns the most-used entries, so a fresh summon shows recents. |
| `Apps.launch(id)` | Starts the entry and records the use. Returns `false` (and logs) if the shell refused, which normally means it was uninstalled since the last scan. |
| `Apps.runCommand(command)` | Hands the string to the shell as if typed into Run, so a query nothing matched can still open `wt`, `notepad` or a path. |
| `Apps.iconFor(id)` | The entry's icon as a `data:image/png;base64,...` URL, or `""`. Resolved on demand and cached, so only the rows actually on screen cost anything. |

Ranking is tiered: an exact name, a name prefix, an acronym (`vsc` finds
*Visual Studio Code*), a later word's prefix, a substring, then a substring
of the app id, and only then scattered subsequence matches. The app id sits
above subsequence matching on purpose — on a localised Windows it is the
only place the English name survives, which is what makes `notep` find
*Notatnik* and `calc` find *Kalkulator*.

Launch counts and timestamps persist to `%APPDATA%\Qwin\apps-usage.json`
— deliberately not under the plugins directory, which is watched, where
rewriting a file on every launch would hot-reload the plugin each time it
was used. The bonus is logarithmic in the launch count, decays with age, and
is capped so it re-ranks near-equal matches without ever burying a better
one.

Enumerating the namespace costs ~300 ms warm and ~600 ms cold, so it must
never land on a summon — hence the five-minute staleness window, and the
`run` plugin's scan shortly after startup rather than on first use.

## Shared components

QML files in `<plugins-dir>\shared\` are reusable components: import them
with `import "../shared"` from a plugin folder. The folder is never loaded
as a plugin, and editing a shared file hot-reloads every plugin (the host
rebuilds its QML engine for these reloads, so plugins always see the fresh
component).

Bundled components:

- `Theme.qml` — a singleton for app-wide non-color theming, read from
  config.json's `"theme"` section (a reserved name, like `"enabled"`).
  Currently `fontFamily` (default `Cascadia Code`), which every bundled
  plugin binds for its text. Read once per load: a config.json edit rebuilds
  the engine, so it applies live without watching anything itself.
- `Popup.qml` — an anchored popup window for panel plugins (the Quickshell
  `PopupWindow` pattern): opens just below an item with a short unfold
  animation, closes on click-outside or Escape, clamps to the screen edge.
  Properties: `anchorItem`, `contentWidth`, `contentHeight`, `gap`,
  `backgroundColor`, `borderColor`; functions `open()` / `dismiss()` /
  `toggle()`; read-only `opened`. Content declared inside lands in a padded
  slot. Popups are single-level by design: `PopupState.qml` holds the one
  open popup, so opening any popup closes the incumbent.
- `PopupButton.qml` — the bordered, hoverable action row the bundled popups
  use for their single hand-off action ("Power settings", "Switch network"):
  `label`, `accentColor` / `surfaceColor` / `textColor`, and a `clicked()`
  signal.
- `WifiIcon.qml` — a Canvas-drawn WiFi strength glyph (`percent`, `off`,
  `litColor`, `dimColor`), no icon font needed.
- `WeatherIcon.qml` — a Canvas-drawn weather glyph for a WMO `code` (the
  codes Open-Meteo returns), with `day` swapping sun for moon, plus
  `litColor` / `dimColor` and a `labelFor(code)` helper.
- `Sparkline.qml` — a Canvas-drawn line+area chart over a rolling `values`
  array (oldest first). `maxValue` fixes the vertical scale, `0` auto-scales
  to the window's own peak floored by `minScale`; `lineColor`, `fillColor`
  and `lineWidth` style it.

Components in `shared/` take their colors as properties instead of reading
the `Colors` singleton, so they stay palette-agnostic and reusable.
