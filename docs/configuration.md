# Configuration

On startup Qwin loads plugins from `%APPDATA%\Qwin\plugins\` (created
automatically), or from the folder given with `--plugins-dir`.

`<plugins-dir>\config.json` is the app-wide configuration. Its `enabled`
array lists which plugins are loaded at top level — with a window of their
own, or, for a `Service` root, with none at all; every other top-level key is
a config section for the plugin of that name, with a shape that plugin
defines for itself:

```json
{
    "enabled": ["bar", "launcher", "run", "tiling"],
    "theme": { "fontFamily": "Cascadia Code" },

    "bar": {
        "primary": {
            "left": ["start", "workspaces", "tiling-indicator", "window-title"],
            "center": ["clock"],
            "right": ["weather", "system-stats", "wifi", "bluetooth", "volume",
                      "battery", "power"]
        },
        "secondary": {
            "left": ["workspaces", "tiling-indicator", "window-title"],
            "center": ["clock"]
        }
    },

    "clock": { "format": "ddd dd MMM  hh:mm:ss" },
    "run": { "hotkey": "Alt+Space", "maxResults": 8 },
    "weather": { "latitude": 52.2297, "longitude": 21.0122, "name": "Warsaw" }
}
```

Plugins left out of `enabled` (like the bar modules above) can still be
embedded by other plugins — the bar assembles its slots from the names in
its section. Without a `config.json` (or without an `enabled` key) every
installed plugin is enabled, so simply dropping a folder in works.

### One bar per monitor

The bundled `bar` plugin puts a `PanelWindow` on every monitor
`System.screens` reports (primary first), and the primary and secondary
monitors' module lists are configured separately under `"bar"`:

- `"primary"` — the primary monitor's `left`/`center`/`right` lists. If
  `"primary"` is missing, `"left"`/`"center"`/`"right"` are read straight off
  `"bar"` itself, so a `config.json` written before per-monitor bars existed
  keeps working unchanged.
- `"secondary"` — the lists for every other monitor's bar. Missing entirely
  gets a built-in slim default, `{ "left": ["workspaces",
  "tiling-indicator", "window-title"] }`; set it to `false` to show no bar at
  all on non-primary monitors. Missing slots (in either section) are empty
  lists.

Per-monitor bar modules (`workspaces`, `tiling-indicator`, `window-title`)
describe the monitor the bar they're embedded in is docked to, not
necessarily the tiler's focused one — so the workspace row on a second
monitor's bar shows that monitor's own workspaces.

The file is watched: editing it applies live, and invalid JSON keeps the
last good config with a log warning. `plugins/config.json` is a complete
working example, and each bundled plugin documents its own section in its
header comment.

## Theming

`<plugins-dir>\colors.json` defines the shared palette (`background`,
`surface`, `text`, `textMuted`, `accent`, `warning`, `error`, plus any keys
you add), and saving it recolors every plugin live — see
[Colors](api.md#colors-theming). The `"theme"` config section above holds
the non-color bits (currently the font the bundled plugins use).
