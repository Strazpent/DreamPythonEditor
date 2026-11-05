# animtk.ui widget reference

A widget is created with a function from `animtk.ui`. Keyword arguments become widget keys, except `on_<event>`
handlers, which stay in Python. Colours are `[r, g, b]` or `[r, g, b, a]` (linear, 0 to 1) or `"#RRGGBB"` /
`"#RRGGBBAA"`. Icons are an editor style name (`"Icons.Plus"`, `"Icons.Refresh"`, `"Icons.Save"`, ...) or a `.png` /
`.svg` file path.

## Keys every widget takes

| Key | Meaning |
|---|---|
| `id` | name for `set()` / `get()`; widgets with handlers get one automatically |
| `tooltip` | hover text |
| `enabled`, `visible` | `False` greys out / collapses the widget (change live with `set`) |
| `width`, `height`, `min_width`, `min_height` | size in Slate units |
| `menu` | right-click menu: a list of `ui.menu_item`, `ui.menu_separator`, `ui.menu_header`, `ui.submenu`; picking an entry fires `menu` with `action` (and `item` / `items` for views and the canvas) |
| `accept_drop` | `True` accepts files from the OS and assets from the Content Browser; fires `drop` with `files` and `assets` (object paths) |

Slot keys, read by the parent box: `stretch` (share of the free space; views fill by default, simple controls do
not), `padding` (number, `[h, v]` or `[l, t, r, b]`; default 2), `halign` / `valign` (`"left"`, `"center"`,
`"right"`, `"fill"` / `"top"`, `"center"`, `"bottom"`, `"fill"`).

## Containers

| Function | Keys | Events |
|---|---|---|
| `VBox(*children)` / `HBox(*children)` | `background` | |
| `Group(*children)` | panel background box; `background`, `inner_padding` | |
| `Scroll(*children)` | `orientation` (`"vertical"` / `"horizontal"`) | |
| `Wrap(*children)` | flows and wraps like a shelf; `spacing` | |
| `Grid(*children)` | `columns`, `spacing` | |
| `Splitter(*children)` | `orientation`; child key `size` (relative) | |
| `Section(title, *children)` | collapsible; `collapsed` | `changed` (`value`: expanded) |
| `Tabs(*children)` / `Tab(label, *children)` | `value` = active index | `changed` (`value`: index) |

## Controls

| Function | Keys | Events |
|---|---|---|
| `Label(text)` | `style` (`"title"`, `"small"`), `bold`, `size`, `color`, `wrap`, `code` | |
| `Button(text)` | `icon`, `icon_size`, `icon_above`, `color`, `text_color`, `flat`, `content_padding`, `style` | `clicked` (`shift`, `ctrl`, `alt`) |
| `Toggle(text, value)` | as Button | `changed` (`value`: bool, modifiers) |
| `CheckBox(text, value)` | | `changed` (`value`: bool) |
| `TextField(value, hint)` | `read_only` | `changed` (every edit), `committed` (Enter or focus loss) |
| `TextArea(value, hint)` | `code` (monospace), `wrap`, `read_only` | `changed` |
| `Slider(value, min, max)` | `step`, `orientation` | `begin` (press), `changed` (drag), `end` (release) |
| `Spin(value, min, max)` | `integer`, `step` | `changed` (drag/typing), `committed` |
| `Combo(options, value)` | | `changed` (`value`, `index`) |
| `Progress(value)` | 0 to 1 | |
| `Image(icon, size)` | | |
| `Separator()` / `Spacer(size)` | `orientation` | |

## Views

`items` are strings or dicts: `{"id", "label", "icon", "color", "tooltip"}`. `selected` is a list of ids.

| Function | Keys | Events |
|---|---|---|
| `ListView(items, multi=True)` | `selected`, `menu` | `selection` (`items`), `double_click` (`item`), `menu` (`action`, `item`, `items`) |
| `TileView(items, tile_size=96, multi=False)` | `icon` is the thumbnail image file; items without one show initials | as ListView |

Change `items` or `selected` live with `set(id, "items", [...])`. Selection kept by id survives an items update.

## Canvas (picker surface)

`Canvas(items, canvas_size=(400, 600))`, items: `{"id", "x", "y", "w", "h", "color", "label", "shape"}` where x/y is
the centre in canvas units and `shape` is `"rect"` or `"circle"`.

| Key | Meaning |
|---|---|
| `selected` | ids drawn as selected |
| `edit` | edit mode: dragging moves items (`moved` event) instead of selecting |
| `snap` | grid snap in canvas units while dragging in edit mode |
| `background` | image file drawn behind the items |
| `fit` | `set(id, "fit", True)` refits the view |

Mouse: click, Shift+click, Ctrl+click, drag a marquee, double-click, middle-drag or Alt+drag to pan, wheel to zoom,
F to fit. Events: `click` (`item`, "" for empty space; modifiers), `marquee` (`items`; modifiers), `double_click`
(`item`), `moved` (`items`: `[{"id", "x", "y"}]`), `menu`, `key` (`key`).

## Panels and tools

- `ui.Tool` (subclass it): `panel_id`, `title`, `tick_interval`, `floating`; class methods `show()`, `register()`,
  `instance()`; methods `build()`, `rebuild()`, `set()`, `get()`, `close()`, `on_open()`, `on_close()`, `tick()`.
- `ui.Panel(panel_id, title)`: the lower-level object a Tool owns. Methods `set_root(widget)`, `show(floating)`,
  `close()`, `is_open()`, `set()`, `get()`, `on(id, event, handler)`, `register_tab(factory_command)`, `destroy()`.
- Dialogs: `ui.confirm(title, message)` (modal Yes/No), `ui.message(title, text)`, `ui.prompt(title, label,
  callback, default)` (non-modal text input).

The C++ side is `unreal.AnimTKPanel` (`find_or_create_panel`, `set_spec`, `set_prop`, `get_value`, `show`,
`register_tab`, `on_event`, `on_closed`) and `unreal.AnimTKLibrary` (`notify`, `get_modifier_keys`,
`clipboard_copy`, `clipboard_paste`). The spec is `{"title": ..., "root": <widget>}`, where a widget is
`{"type", "id", ...keys, "children": [...]}`.
