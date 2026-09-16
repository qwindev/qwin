# Writing a plugin

For end users: drop a folder into the plugins directory (see
[Configuration](configuration.md)) and it loads. This doc is for writing
that folder.

## Anatomy

A plugin is a folder directly inside the plugins directory, holding a
`manifest.json` and a `plugin.qml`:

```
plugins/
  weather/
    manifest.json
    plugin.qml
```

The registry keys plugins by the manifest's `name`, not the folder name, but
name the folder after the plugin anyway — it is what shows up in file
listings and zip extracts. Plugin-local widgets and glyphs (a popup button, a
weather glyph, a sparkline) are declared as inline components
(`component Name: ...`) inside `plugin.qml` rather than split into extra
files, so a folder stays self-contained.

A plugin must not reach into another plugin's folder (`import "../other"`
or similar) — folders get separated the moment they're published as
individual repositories (see [Publishing](#publishing)). To embed another
plugin, go through the registry instead:
`Loader { source: Plugins.source("other-name") }` — see
[`Plugins`](api.md#plugins-registry--config).

## manifest.json

```json
{
    "name": "weather",
    "version": "1.2.0",
    "author": "Jane Doe",
    "description": "Temperature and a 3-day forecast from Open-Meteo.",
    "repository": "qwindev/weather",
    "minQwinVersion": "1.0.0"
}
```

| Field | Required | Notes |
|---|---|---|
| `name` | yes | Dashed lowercase (letters, digits, dashes). Must be unique among installed plugins. It is also the `config.json` section key for this plugin (see [Configuration](#configuration)) and the name other plugins use to embed it. `"enabled"` and `"theme"` are reserved and cannot be used as a plugin name. |
| `version` | no | Free text, e.g. `"1.2.0"`. Not validated or compared to anything — `minQwinVersion` below is what gates loading. |
| `author` | no | Free text. |
| `description` | no | One line describing the plugin. Must be a JSON string if present. |
| `repository` | no | The GitHub repo this plugin is published from, as `"owner/repo"` (e.g. `"qwindev/weather"`) — not a full URL. |
| `minQwinVersion` | no | The oldest Qwin this plugin works with, e.g. `"1.2.0"`. |

A manifest that fails validation — a missing or invalid `name`, a field of
the wrong type, a malformed `repository` or `minQwinVersion` — never loads
`plugin.qml`. The host shows a built-in error window in its place instead,
with the validation message on screen, keyed to the plugin's folder; fixing
the file swaps the real plugin back in without a restart.

The same happens when `minQwinVersion` names a Qwin newer than the one
running: the error window reads
`Needs Qwin <required> or newer - this is Qwin <running>.` with a link to
the releases page. A local dev build (not built from a release tag) reports
version `0.0.0` and skips this check entirely, so it always loads plugins
regardless of what they declare.

## Root types

A plugin's `plugin.qml` root is one of:

- `Window` / [`PanelWindow`](api.md#panelwindow-taskbar-style-panels) — the
  plugin owns its own window (a docked bar, a floating widget).
- `Item` — either embedded by another plugin's `Loader`, or, if enabled on
  its own, wrapped in a default frameless window automatically.
- [`Service`](api.md#service-windowless-plugins) — no window at all; only
  hotkeys, config pushes, timers.

## Imports

```qml
import Qwin
```

is the plugin API — see [the API reference](api.md) for everything it
provides (singletons, `Hotkey`, `PanelWindow`, `Service`).

```qml
import Qwin.Ui
```

is the bundled UI components — see
[UI components](api.md#ui-components-import-qwinui) (`Theme`, `Popup`,
`PopupState`).

Beyond those two, packaged builds only guarantee `QtQuick`, `QtQuick.Window`,
`QtQuick.Controls` and `QtQuick.Layouts` (see [Packaging](packaging.md) —
these are what `windeployqt` is told to bundle at build time). Anything
else you import may simply be missing on a user's machine; stick to that
list unless you know your users can install more Qt modules themselves.

## Configuration

Read your own section of `config.json` with `Plugins.config(name)` (see
[`Plugins`](api.md#plugins-registry--config)) — give every key you read a
default, since the section is `{}` when absent:

```qml
readonly property var cfg: Plugins.config("weather")
readonly property real latitude: cfg.latitude ?? 52.2297
```

Document the section's shape in `plugin.qml`'s header comment — every
bundled plugin does this, e.g.:

```qml
// config.json section (all keys optional):
//   "weather": { "latitude": 52.2297, "longitude": 21.0122,
//                "refreshMinutes": 15 }
```

A plugin is loaded either by listing its name in `config.json`'s `enabled`
array (or simply by being installed, if there's no `config.json` / no
`enabled` key at all — see [Configuration](configuration.md)), or by being
named inside a container plugin's own section, the way the bundled `bar`
lists its modules.

## Hiding and degrading

A module with nothing to report on this machine (no battery, no Bluetooth
radio, nothing playing) should declare `property bool shown` on its root
rather than always drawing an empty slot — see the note in
[`Plugins`](api.md#plugins-registry--config) about binding a container's
`Loader.visible` to it (not the loaded item's own `visible`, which
deadlocks).

## Debugging

Saving any file in a loaded plugin's folder hot-reloads it in place,
usually within about a second. A broken manifest or a QML error shows the
same built-in error window described above, with the error on screen —
fix the file and the reload swaps the real plugin back in.

The Release exe has no console, so `console.log()` output and QML warnings
go nowhere by default. To see them, quit Qwin from the tray (a second
instance exits immediately while one is running), then start it from the
folder holding `qwin.exe` with stderr logging forced and redirected to a
file:

```powershell
$env:QT_FORCE_STDERR_LOGGING = "1"
Start-Process -FilePath .\qwin.exe -RedirectStandardError qwin.log
```

then read `qwin.log` (or `Get-Content -Wait qwin.log` to follow it live).

## Publishing

Qwin has no plugin store; distribution is plain GitHub repos.

- One plugin per repository, with `manifest.json` and `plugin.qml` at the
  repo root (not nested in a subfolder).
- Set `"repository"` in the manifest to that repo's `"owner/repo"`.
- Add the `qwin-plugin` GitHub topic to the repo so people can find it.
- Tag releases `vX.Y.Z` matching the manifest's `"version"`.
- Set `"minQwinVersion"` to the oldest Qwin you actually tested against.

To install one today: download the release's source zip and extract it so
the folder holding `manifest.json` sits directly inside
`%APPDATA%\Qwin\plugins\`. GitHub's zips already wrap the files in a
`repo-version` folder, and Windows' "Extract All" by default puts that inside
another folder named after the zip, which leaves `manifest.json` one level
too deep to be found — move the inner folder up (renaming it to the
plugin's name). Then either add the name to
`config.json`'s `"enabled"` array or a container's section (e.g. the bar),
or, with no `config.json` at all, it is enabled automatically.

Plugins are code with the full Qwin API — shutting down the machine,
launching apps, opening any file or URL, network access via
`XMLHttpRequest` — so only install ones you trust, the same as any other
executable.
