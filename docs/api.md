# Plugin API reference

New to writing a plugin? Start with [Writing a plugin](plugins.md) —
manifest.json, project layout, imports, publishing.

Everything a plugin can use comes from one import:

```qml
import Qwin
```

Singletons: [`System`](#system) · [`Plugins`](#plugins-registry--config) ·
[`Colors`](#colors-theming) ·
[`Tiler`](#tiler-window-tiling) · [`Wifi`](#wifi) · [`Media`](#media-now-playing) ·
[`Audio`](#audio-volume--output-devices) · [`ActiveWindow`](#activewindow-focused-window) ·
[`Battery`](#battery) · [`Power`](#power) · [`Bluetooth`](#bluetooth) · [`Apps`](#apps-installed-apps) ·
[`Hotkeys`](#hotkeys-hotkey-listing)

Types: [`Hotkey`](#hotkey-global-hotkeys) · [`PanelWindow`](#panelwindow-taskbar-style-panels) ·
[`Service`](#service-windowless-plugins)

Plus the [UI components](#ui-components-import-qwinui) from `import Qwin.Ui`.

## System

| Member | Description |
|---|---|
| `System.cpuUsage` | Total CPU usage in percent, refreshed every second (`statsChanged`). |
| `System.memoryUsagePercent` | Physical memory load in percent, refreshed every second. |
| `System.hostname` | Machine host name (constant). |
| `System.readTextFile(path)` | Returns the file content as a string. Relative paths resolve against the plugins directory; paths escaping the plugins directory (after canonicalization) are rejected and return `""`. |
| `System.openStartMenu()` | Opens the Windows Start menu, or closes it again — it toggles, like the key it synthesizes (Ctrl+Esc). |
| `System.rememberFocus()` / `System.restoreFocus()` | Focus bracket for overlays and popups: call `rememberFocus()` before taking the keyboard (showing a popup or overlay), `restoreFocus()` after hiding, so the window the user was working in gets the keyboard back. `Qwin.Ui`'s `Popup` and the bundled `run`/`launcher` overlays use it. |
| `System.screens` | One entry per live monitor, primary first: `{ device, name, primary }`. `device` is the GDI device name (`\\.\DISPLAY5`) that `PanelWindow.screenName`/`.device` and the `Tiler` singleton key by; `name` is the friendly name (`Screen.name`, display only — see the note below). Updates on `screensChanged` (a monitor plugged/unplugged, or the primary reassigned). |

Iterate `System.screens`, never `Qt.application.screens`, when a plugin needs
to address a specific monitor: `Screen.name`/`QScreen::name()` return a
*friendly* name on this host ("LG HDR WQHD"), which is not even unique
between two identical monitors, and is not the identifier anything else in
Qwin keys by.

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

Three root kinds, in short: `Window` / `PanelWindow` (the plugin owns its
own window), `Item` (embedded by a container's `Loader`, or wrapped in a
default frameless window when it is enabled on its own instead), `Service`
(no window at all — see below).

A module that wants to hide itself (e.g. a WiFi module on a machine with no
adapter) declares `property bool shown` on its root, and the container binds
its Loader slot's `visible` to that. Do not bind on the loaded item's
`visible` for this: it reports effective visibility (parent chain included),
which feeds back through the Loader and deadlocks invisible.

`Plugins.config()` values are plain snapshots, not live bindings — editing
`config.json` reloads the affected plugins instead, so reading the config
once at load is always in sync. Changing a plugin that others embed reloads
the embedders too (via a full engine rebuild).

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
the `Theme` singleton (see
[UI components](#ui-components-import-qwinui)).

## Hotkey (global hotkeys)

Registers a **system-wide** keyboard shortcut via the Win32 `RegisterHotKey`
API — it fires no matter which application has focus, unlike QtQuick's
`Shortcut`, which only works while the plugin's own window is focused:

```qml
Hotkey {
    sequence: "Shift+Alt+1"        // modifiers + one key, Qt shortcut syntax
    onActivated: Tiler.switchToWorkspace(0)
}
```

| Member | Description |
|---|---|
| `sequence` | The chord, e.g. `"Shift+Alt+1"`, `"Ctrl+F9"`, `"Meta+Space"`. Modifiers: `Ctrl`, `Alt`, `Shift`, `Meta` (the Windows key); one main key (letter, digit, F1–F24, arrows, and other common keys). |
| `enabled` | Set to `false` to release the chord without removing the declaration (default `true`). |
| `registered` | Read-only: whether the OS registration succeeded. |
| `description` | Free text describing what the chord does. No effect on registration — purely for a listing like the bundled `hotkeys` plugin. |
| `activated()` | Emitted on every press of the chord (once per press, no auto-repeat). |

- The chord is grabbed **exclusively** while registered: the focused
  application never sees it. Pick combinations that don't collide with keys
  you type.
- If another application (or another plugin) already owns the chord,
  registration fails with a log warning and `registered` stays `false` — the
  plugin itself keeps working.
- The registration is released automatically when the plugin is removed,
  hot-reloaded, or the app quits.

## Hotkeys (hotkey listing)

A snapshot of every `Hotkey` currently declared anywhere in the loaded
plugins:

```qml
function summon() {
    entries = Hotkeys.list()
    // ...
}
```

`Hotkeys.list()` returns an array of `{sequence, plugin, description,
registered}`, sorted by `plugin` then by `sequence` (both case-insensitive).
`sequence` is the display form of the chord, with `Meta` shown as `Win`;
`plugin` is the name of the plugin whose *folder* declared the `Hotkey` (an
embedded bar module reports its own name, not `bar`); `registered: false`
means the chord was taken by another application or could not be parsed —
the log has the detail in either case.

It is a **snapshot, not a live list** — call it when you need it, e.g. when
an overlay opens, rather than binding to it. There is deliberately no change
signal: a hot reload destroys and recreates every `Hotkey` at once, and a
caller that reads on open never sees that churn. Disabled hotkeys and ones
with an empty `sequence` (a `Hotkey` an `Instantiator` is mid-teardown on)
are omitted.

The bundled `hotkeys` plugin is a cheat sheet built entirely on this: a
hotkey (default `Shift+Alt+/`) summons a filterable, grouped-by-plugin list
of everything `Hotkeys.list()` returns.

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

| Member | Description |
|---|---|
| `screenName` | Read/write. The monitor to dock on, as a `device` from [`System.screens`](#system) — never `Screen.name` (a friendly name, not unique, and not what this matches against). Empty (the default) keeps the panel on whatever monitor it ends up on, following it if it moves — today's behaviour before this property existed. |
| `device` | Read-only. The `device` of the monitor the panel is actually docked on right now (the resolved `screenName`, or the monitor `MonitorFromWindow` answers when `screenName` is empty). Embedded modules read their bar's monitor through `Window.window.device`. |

If `screenName` names a monitor that is not currently plugged in, the panel
releases its AppBar reservation and waits — it never falls back to another
monitor, which would otherwise stack a second bar on the primary every time
the named one is unplugged. It picks the monitor back up automatically if it
reappears.

## Service (windowless plugins)

Use `Service` as the plugin root for a plugin that has nothing to show — it
only binds hotkeys, pushes config into a singleton, or runs a timer. The
bundled `tiling` plugin is one: it configures the `Tiler` singleton and
binds every tiling and workspace chord, with no window of its own.

```qml
import QtQuick
import Qwin

Service {
    Hotkey {
        sequence: "Shift+Alt+T"
        onActivated: Tiler.enabled = !Tiler.enabled
    }
}
```

- Children are non-visual objects: `Hotkey`, `Binding`, `Timer`,
  `Connections`, `Instantiator`, and the like — not `Item`-derived types.
- A `Service` is listed in `enabled`; it is not a module to embed in another
  plugin's `Loader`.
- Hot reload works exactly like any other plugin: the old root is deleted
  before the replacement registers, which is what releases its `Hotkey`
  chords in time for the new ones to claim them.

## Tiler (window tiling)

A dwindle tiling window manager over the desktop's own windows, with
per-monitor **workspaces** layered on top: each new window splits the
focused one along its longer side, dragging an inner edge rewrites the
split behind it, a title-bar drag onto another tile swaps the two, and
fixed-size dialogs are never tiled. Windows that keep refusing their
assigned rect are floated rather than fought with.

Workspaces are internal to the tiler, not the OS. Switching one hides its
previous members and shows the new ones by **cloaking** them — the same
undocumented shell mechanism Windows itself uses to park a window on
another virtual desktop, so a cloaked window disappears from the screen
without being minimized or dropped from Alt+Tab the way minimizing it
would. Native virtual desktops keep working underneath and are left alone:
the tiler only ever discovers and manages windows on the current one.

The layout and workspace state live in C++ and survive hot reloads —
`plugins/tiling` is a windowless `Service` that pushes config in and binds
every tiling and workspace `Hotkey` chord to the commands;
`plugins/tiling-indicator` and `plugins/workspaces` are the bar-facing
display modules (on/off + tile count, and the workspace switcher buttons).

| Member | Description |
|---|---|
| `Tiler.enabled` | Read/write master switch. Disabling shows every hidden window and releases each one that came back (restoring its pre-adoption geometry); the state file is deleted once everything is shown, or kept and retried if a window would not come back. |
| `Tiler.gap` / `Tiler.outerGap` | Gap between tiles / to the work-area edge, logical px. |
| `Tiler.minWidth` / `Tiler.minHeight` | Smallest tile a split may create, logical px. A window that cannot be placed without breaking these stays floating and is reclaimed once room frees up. |
| `Tiler.resizeStep` | How far one `resize()` call moves a divider, logical px. |
| `Tiler.floatProcesses` | Executable names (`"spotify.exe"`, case-insensitive) that are never tiled. |
| `Tiler.workspaceCount` | Workspaces per monitor, 1–20 (default 9). |
| `Tiler.hideMethod` | `"cloak"` (default) or `"minimize"` — how an inactive workspace's windows are hidden. See the hiding notes below for the automatic fallback. |
| `Tiler.cloakAvailable` | Whether the cloaking COM interface is up. `false` means every hide uses minimize regardless of `hideMethod`. Acquisition is lazy and retried, so this can change — notifies through `cloakAvailableChanged`. |
| `Tiler.pinnedTopmost` | Whether a pinned window is also kept above the active workspace's windows (`HWND_TOPMOST`) rather than just following it around. |
| `Tiler.currentWorkspace` | Active workspace index (0-based) on the focused monitor. |
| `Tiler.workspaces` | The focused monitor's workspaces: `[{ index, active, windows }]`, where `windows` is the member count. |
| `Tiler.monitors` | One entry per live monitor, keyed by `device` (so an empty monitor still appears): `{ active, focused, tiles, workspaces }` — `active` that monitor's active workspace index, `focused` whether it is `focusedDevice`, `tiles` its own tile count, `workspaces` exactly the shape the `workspaces` property returns, for that monitor. What a per-monitor bar module binds to instead of the focused-monitor-only properties above. |
| `Tiler.focusedDevice` | The monitor `switchToWorkspace()` and the workspace properties act on: the foreground window's monitor when it names a real one, else the monitor under the cursor, else the primary screen. |
| `Tiler.managedCount` | Number of currently tiled, visible windows (`layoutChanged`). |
| `Tiler.debug` | Log every adoption and every rect applied. Off by default: it is one line per window per re-tile. |
| `Tiler.focusDirection(dir)` | Focus the neighbouring window: `"left"`, `"right"`, `"up"`, `"down"`. At the edge of the focused window's monitor (or when the foreground window is not a tiled member), falls through to `focusMonitor(dir)` — the chord crosses onto the adjacent monitor instead of doing nothing. |
| `Tiler.moveDirection(dir)` | Swap the focused window with its neighbour in that direction. At the same edge `focusDirection` crosses at, falls through to `moveToMonitor(dir)` instead. |
| `Tiler.focusMonitor(dir)` | Crosses onto the monitor adjacent to `focusedDevice` in `dir` and focuses its last-focused or topmost member (or the shell, if it has none). No-op if there is no monitor that way. |
| `Tiler.moveToMonitor(dir)` | Moves the foreground window — if it is one Qwin manages — onto the adjacent monitor in `dir`: tiled there if there is room, otherwise placed at the same relative offset and size (clamped to fit). No-op if there is no monitor that way, or the foreground window is not managed. |
| `Tiler.switchToWorkspaceOn(device, index)` | Like `switchToWorkspace(index)`, but on `device` instead of `focusedDevice` — for a workspace button clicked on a non-focused monitor's bar. No-op if `device` names no live monitor. |
| `Tiler.resize(how)` | Move the divider nearest the focused window: `"wider"`, `"narrower"`, `"taller"`, `"shorter"`. |
| `Tiler.toggleFloating()` | Take the focused window out of the layout (restoring its adopted size), or put it back in. On a window floating only for lack of room, makes that the user's choice instead, so it is no longer reclaimed the moment room frees up. A float survives a minimize. |
| `Tiler.toggleSplit()` | Flip the split that placed the focused window — the one-key fix for a dwindle that divided the wrong way. |
| `Tiler.equalize()` | Forget every resize on the focused window's monitor. |
| `Tiler.retile()` | Re-apply the layout now. |
| `Tiler.switchToWorkspace(index)` | Switch the focused monitor to the given workspace (0-based): shows its members first, then hides the previous workspace's non-pinned members — no empty-desktop flash in between. |
| `Tiler.moveToWorkspace(index, follow = false)` | Move the foreground window to the given workspace on its own monitor; `follow` also switches that monitor to it. |
| `Tiler.moveToEmptyWorkspace()` | Move the foreground window to the lowest-numbered workspace on its monitor that has no windows, and follow it there. Its own workspace counts as occupied, so a window alone on one lands on the next free slot rather than staying put. A warning no-op when every workspace is in use. Hyprland's `movetoworkspace, empty`. |
| `Tiler.togglePinned()` | Pin or unpin the foreground window. A pinned window floats and stays visible on every workspace of its monitor (subject to `pinnedTopmost`) instead of hiding when the workspace switches. |

The **focused monitor** — what `currentWorkspace`, `workspaces`,
`switchToWorkspace()` and the plain (non-`follow`) `moveToWorkspace()` act
on — is the monitor of the foreground window when that window is managed,
else the monitor under the cursor.

A window with no `WS_THICKFRAME` (a fixed-size dialog, an installer, a
splash) always floats rather than tiling — otherwise dialogs would bleed
across workspaces. Everything else on the current native desktop is
managed, and `floatProcesses` is the one way to exempt an application from
the layout.

**Hiding**: cloaking is invisible-but-present, which is why it's the
default — the window keeps its place in Alt+Tab and its taskbar button.
When `SetCloak` keeps failing for a specific window (three times), that
window falls back to minimize for the rest of the session; when the
cloaking COM interface never came up at all (`cloakAvailable == false`),
every hide uses minimize from the start. The tiler never uncloaks a window
it did not cloak itself, so windows the OS itself cloaks — for another
native virtual desktop, or a dormant UWP host — are left untouched.

**State and recovery**: the tiler writes `%APPDATA%\Qwin\tiling-state.json`
before every hide/show batch (so a forced kill mid-switch can never leave
the file claiming a window is visible when it's actually cloaked) and on a
short debounce after other layout changes. There is no crash handler and
nothing runs at the moment a crash happens — recovery is entirely a startup
affair. On the *next* launch, `TilingApi`'s constructor reads the file back,
matches each entry to a live window by PID, process creation time and
window class (never the raw HWND, which Windows can recycle), and
reconciles that window's hidden/visible state to what its recovered
workspace and active-workspace map imply — a forced kill is recovered from
on the next launch, not at the moment it happens. A missing or unparseable
file is simply logged. A wrong-version or clearly stale (boot-time mismatch)
one is not rebuilt from — but it is still read far enough to un-hide every
window it names that passes the identity check, since otherwise a file the
tiler refuses to trust would be the one thing that could strand a window
cloaked. A structurally broken one
(a workspace tree or window list that doesn't add up) is abandoned the same
way — everything it named that was hidden is shown again, and the file is
deleted once every one of them came back (whatever doesn't stays pending and
is retried, same as a normal recovery).
Recovered windows are held pending until the plugin's own `enabled` binding
switches the tiler on, so a profile that starts with tiling disabled can
never leave one stranded cloaked — if that never happens, a 5 s timeout
shows them instead, retrying any that will not come back. A clean exit (tray Quit —
killing the process directly skips this) shows and un-topmosts everything the
tiler hid and deletes the file once everything came back; anything that
would not is left for the next launch to finish.

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

## Battery

| Member | Description |
|---|---|
| `Battery.available` | Whether the machine has a battery at all (`false` on desktops — the bundled `battery` plugin hides itself on that). |
| `Battery.percent` | Charge 0–100, or `-1` when Windows reports it as unknown. |
| `Battery.charging` | Whether the battery is actively charging. |
| `Battery.acPower` | Whether the machine is running on mains. Distinct from `charging`: plugged in and full is `acPower` without `charging`. |
| `Battery.timeLeft` | Estimated seconds of runtime left, or `-1` when unknown (which includes being on AC). |
| `Battery.saver` | Whether Windows battery saver is on. |

Polled once a second; `changed` fires only when something actually moved,
since battery state rarely changes and a per-second signal would re-run
every `Battery` binding for nothing.

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

## UI components (`import Qwin.Ui`)

`Theme`, `Popup` and `PopupState` are host API, compiled into the `qwin`
exe from `src/ui/` and versioned with it — not sample content under
`plugins/`. Import them with:

```qml
import Qwin.Ui
```

There is no hot reload for this module: it changes only on a rebuild of the
exe itself.

- `Theme` — a singleton for app-wide non-color theming, read from
  config.json's `"theme"` section (a reserved name, like `"enabled"`).
  Currently `fontFamily` (default `Cascadia Code`), which every bundled
  plugin binds for its text. Read once per load: a config.json edit rebuilds
  the engine, so it applies live without watching anything itself.
- `Popup` — an anchored popup window for panel plugins (the Quickshell
  `PopupWindow` pattern): opens just below an item with a short unfold
  animation, closes on click-outside or Escape, clamps to the screen edge.
  Properties: `anchorItem`, `contentWidth`, `contentHeight`, `gap`,
  `backgroundColor`, `borderColor`; functions `open()` / `dismiss()` /
  `toggle()`; read-only `opened`. Content declared inside lands in a padded
  slot.
- `PopupState` — the singleton `Popup` claims on open and releases on close,
  application-wide, so opening any popup closes whichever one was already
  open without the plugins knowing about each other. This is what makes
  popups single-level: every plugin must share the one copy of `PopupState`
  for the rule to hold, which is why it is compiled into the exe rather than
  vendored per plugin.

`Popup` takes its colors as properties instead of reading the
`Colors` singleton, so it stays palette-agnostic and reusable.

Plugin-specific widgets and glyphs (a popup action button, a WiFi or weather
glyph, a sparkline) live as inline components in their own plugin
(`component Name: ...`), not here, so a plugin folder stays self-contained.
