# Configuration

On startup qwin loads plugins from `%APPDATA%\qwin\plugins\` (created
automatically), or from the folder given with `--plugins-dir`.

`<plugins-dir>\config.json` is the app-wide configuration. Its `enabled`
array lists which plugins get their own window; every other top-level key is
a config section for the plugin of that name, with a shape that plugin
defines for itself:

```json
{
    "enabled": ["bar", "launcher", "run"],
    "theme": { "fontFamily": "Cascadia Code" },

    "bar": {
        "left": ["start", "workspaces", "tiling", "window-title"],
        "center": ["clock"],
        "right": ["weather", "system-stats", "wifi", "bluetooth", "volume",
                  "battery", "power"]
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
