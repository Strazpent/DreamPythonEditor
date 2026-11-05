# Anim Toolkit

Python wrappers around Sequencer, Control Rig and Slate for Unreal Engine 5.8, so technical animators can build
animation tools in Python and put them on a Maya-style shelf.

It ships with:

- **Anim Shelf**: tabs of buttons that run Python, like Maya shelves.
- **Rig Picker**: click controls on a 2D layout of any Control Rig.
- **Pose Library**: save, apply, blend and mirror poses and short clips.
- **Selection Sets**: one-click groups of controls.
- **Anim Toolbelt**: tween/blend slider, key tools, timing, pose and selection tools, in the spirit of AnimBot.
- **Character Loader**: pick a character and load its mesh and Control Rig into the sequence, ready to animate (a rig
  referencer).

Every tool is plain Python built on the same public API, so each one also works as an example to copy.

## Install

1. Copy `Plugins/AnimToolkit` into your project's `Plugins` folder (or add its parent folder to
   `AdditionalPluginDirectories`).
2. Enable **Anim Toolkit** in *Edit > Plugins*. It turns on Python Editor Script Plugin, Editor Scripting Utilities,
   Sequencer Scripting and Control Rig. The plugin is off by default, so it never loads in a project that did not ask
   for it.
3. Restart the editor. A code project builds the C++ module on start. A Blueprint-only project needs the plugin
   built first; run the command below once, with the paths changed to yours:

```bash
"C:/Program Files/Epic Games/UE_5.8/Engine/Build/BatchFiles/Build.bat" UnrealEditor Win64 Development -Project="C:/Path/To/YourProject.uproject" -WaitMutex
```

After the restart you get an **Anim Toolkit** main menu, an **Anim Shelf** toolbar button, and every tool under
*Window > Tools > Anim Toolkit*. Docked tools come back with the editor layout.

## How it fits together

```
Python (Content/Python/animtk)                         C++ (Source/AnimToolkit)
------------------------------                         ------------------------
tools/*.py  example tools  ─┐
shelf.py    script shelf    ├─ ui.py ── JSON spec ───▶ UAnimTKPanel   one dockable tab per panel
                            │          ◀── events ───  FAnimTKWidgetTree  Slate widgets built from the spec
ops.py  pose.py             │                          SAnimTKCanvas  picker surface (draw, click, marquee, drag)
rig.py  seq.py  core.py  ───┴─ unreal.ControlRigSequencerLibrary, LevelSequenceEditorBlueprintLibrary, ...
```

Python cannot create Slate widgets directly. The C++ module turns a declarative description into real Slate widgets
and sends widget events back to Python as `(widget id, event, JSON payload)`. That keeps tool code short and free of
C++, and the widgets are real Slate widgets that dock, theme, scale and undo like the rest of the editor.

| Module | What it wraps |
|---|---|
| `animtk.core` | logging, toasts, undo (`with core.undo("..."):`), drag-long transactions, editor world, assets, user data paths, JSON files, tick callbacks |
| `animtk.seq` | the open Level Sequence, playhead, playback/selection/work range, bindings, adding actors, key times, selected keys |
| `animtk.rig` | `Rig`: one Control Rig track; its controls, kinds, colours, rest positions, hierarchy, selection, typed values (get/set/many/world), keys, channels |
| `animtk.pose` | poses and clips as JSON data: capture, apply with weight, lerp, mirror (configurable naming rules), an in-memory clipboard |
| `animtk.ops` | animator operations on the selection: engine blend modes (Tween, Blend to Neighbor, Push/Pull, Blend Relative, Blend to Ease, Smooth/Rough), live blend drags, key, delete, nudge, share keys, bake on 1s/2s/3s, stagger, next/previous key, reset, mirror, copy/paste pose, selection helpers, anim layers |
| `animtk.ui` | declarative Slate: `ui.Tool`, containers, controls, list/tile views, the picker canvas, context menus, drag and drop, prompts and confirms |
| `animtk.shelf` | the shelf: model (`ShelfStore`), runner, panel, button editor, `shelf.add_button(...)` |

Frames are always display-rate frame numbers, the numbers the Sequencer timeline shows.

## Writing a tool

```python
import animtk.core as core
import animtk.ops as ops
import animtk.rig as rig
import animtk.ui as ui


class OffsetTool(ui.Tool):
    panel_id = "offset_tool"        # one panel (tab) per id
    title = "Offset Keys"
    tick_interval = 0.25            # tick() runs this often while the tab is open

    def build(self):
        return ui.VBox(
            ui.Label("", id="info", style="small"),
            ui.HBox(ui.Label("Frames"), ui.Spin(2, -100, 100, integer=True, id="frames")),
            ui.Button("Offset Selected Keys", color="#2E86C1", on_clicked=self.offset),
        )

    def tick(self):
        self.set("info", "text", "{} control(s) selected".format(len(rig.selected_controls())))

    def offset(self, event):
        frames = int(self.get("frames"))
        moved = ops.nudge(-frames if event.shift else frames)   # Shift+click goes the other way
        core.notify("Moved {} key(s)".format(moved))


OffsetTool.show()          # or OffsetTool.register() to list it under Window > Tools > Anim Toolkit
```

- Handlers are `on_<event>=` keywords. They receive an `Event` (`event.value`, `event.item`, `event.items`,
  `event.shift`, `event.ctrl`, `event.alt`, `event.action`, `event.files`, `event.assets`). A handler that takes no
  argument is called without one.
- `self.set(id, prop, value)` changes one widget live; `self.get(id)` reads its value; `self.rebuild()` rebuilds the
  whole panel from `build()`.
- Raise `core.AnimTKError("message")` to tell the animator what went wrong. It shows as a toast, not a traceback.
  Any other exception in a handler is logged with its traceback and never reaches Slate.
- Wrap edits in `with core.undo("Label"):` so they make one undo step. For slider drags use
  `core.Interaction` (`begin()` on press, `end()` on release), or `ops.BlendDrag` for blends.
- *Anim Toolkit > New Tool From Template...* copies `Resources/Templates/my_tool.py` into your scripts folder, puts a
  button for it on the shelf and opens it. Edit the file, save, press the button again.
- *Anim Toolkit > Reload Anim Toolkit* (or `animtk.reload()`) re-imports every `animtk` module and reopens open tools,
  so you can edit a tool without restarting the editor.

[docs/UI_REFERENCE.md](docs/UI_REFERENCE.md) lists every widget with its keys and events.

## The shelf

- Click a button to run it. A button runs inline Python (`command`) or a `.py` file (`script`), in a fresh
  namespace with `unreal` and `animtk` already imported, as one undo step.
- **Add buttons:** drag `.py` files from Explorer onto the shelf, or right-click it and choose *New Button...* or
  *Paste Button From Clipboard*. Dragging Content Browser assets onto the shelf adds "open asset" buttons. Scripts
  can call `animtk.shelf.add_button(...)`.
- **Edit buttons:** right-click a button to edit, duplicate, recolour, move or delete it, or to open its script (in
  the Dream Python Editor when it is installed).
- **Shelf tabs:** click to switch. Right-click to rename, move or delete a shelf. **+** adds one.
- **Where shelves live:** your shelves are in `<Project>/Saved/AnimToolkit/shelves.json`, seeded from
  `Resources/Shelves/default_shelves.json`. Studio shelves are read from every folder in `ANIMTK_SHELF_PATH`, one
  shelf per `*.json` file. They are read-only; *Copy To My Shelves* gives you an editable copy.
- **Script paths:** a relative `script` path is looked up in `Saved/AnimToolkit/Scripts`, then in
  `Resources/ShelfScripts`. Environment variables and `~` are expanded.

## The example tools

**Rig Picker** (`tools/picker.py`)
- Choose a rig from the combo; with no saved picker, it generates one from the rig. The layout comes from the
  controls' rest positions and their shape colours. Left and right are read from the control names, so the
  character's left is on screen right however the rig is oriented.
- Click selects, Shift adds, Ctrl toggles. Drag a box to marquee select. Double-click selects a control and
  everything under it. The picker follows the selection you make in the viewport.
- **Edit** mode: drag buttons around (with snapping). Right-click to recolour, resize, reshape, label or delete
  them, or to add the selected controls or mirror the layout. Use *Auto Front* / *Auto Side* to regenerate, and
  *Save* to keep it.
- Pickers are saved per Control Rig asset in `Saved/AnimToolkit/Pickers`. Studio pickers in `ANIMTK_PICKER_PATH`
  are used first. An optional `background` image goes behind the buttons.

**Pose Library** (`tools/pose_library.py`)
- Folders on the left, thumbnails on the right, and a search box.
- *Save Pose* stores the selected controls (with *Selected only*) or the whole rig at the playhead. *Save Anim*
  stores the work range: the Sequencer selection range, or else the playback range.
- Double-click or *Apply* to apply. The blend slider fades from the current pose into the selected pose in one undo
  step. Use *Mirror* for the opposite side; turn off *Key* to try a pose without keying it.
- Right-click a pose to apply it mirrored, select its controls, re-capture its thumbnail, overwrite, rename or
  delete it.
- Each pose is one JSON file plus a PNG thumbnail. Your library is in `Saved/AnimToolkit/PoseLibrary`; add more
  libraries with `ANIMTK_POSE_LIBRARY_PATH`.

**Selection Sets** (`tools/selection_sets.py`)
- Type a name and press *Create* to make a set from the selected controls.
- Click selects the set, Shift adds it, Ctrl removes it, Alt keys it.
- Right-click a set to add, remove or replace controls with the selection, key it, make a mirrored set,
  recolour, rename, move or delete it.
- Sets remember the rig by name and fall back to any rig with the same controls, so they carry across characters.

**Anim Toolbelt** (`tools/toolbelt.py`)
- The blend slider runs one of the engine's six blend modes on the selected controls at the playhead. A drag is
  one undo step and never accumulates. *Overshoot* widens the range to ±150%. The preset buttons apply ±25, 50, 75
  and 100%.
- **Keys:** key, delete, nudge left and right, share keys, bake on 1s, 2s or 3s, stagger. The *Step* box sets the
  nudge and stagger amount.
- **Timeline:** previous and next key of the selection, and frame steps.
- **Pose:** copy, paste, paste mirrored, reset, mirror.
- **Selection:** all, invert, children, mirror (Shift adds), frame, and add an animation layer from the selection.

**Character Loader** (`tools/character_loader.py`)
- Characters come from JSON registries. Studio registries in `ANIMTK_CHARACTER_PATH` come first, then yours
  (`Saved/AnimToolkit/characters.json`), then `Resources/Characters/characters.json`, which has the UE5 mannequins
  from the Third Person template.
- **Adding characters:** *Scan Project* adds every Control Rig that has a preview mesh. You can also drag Control
  Rig assets from the Content Browser onto the list.
- **Load Into Scene** does four things:
  1. uses the open sequence, or creates and opens `/Game/Cinematics/AnimTK_Scratch`;
  2. spawns the mesh with a namespace label such as `Manny:1`;
  3. adds it to the sequence, as a possessable or a spawnable;
  4. adds a Control Rig track and selects the actor.
- *In This Level* lists what the loader put in the level. You can select those characters, select all their
  controls, reload or swap their rig, or remove them from the level and the sequence.

## Data and environment variables

| Variable | Effect |
|---|---|
| `ANIMTK_USER_DIR` | where per-user data goes (default `<Project>/Saved/AnimToolkit`) |
| `ANIMTK_SHELF_PATH` | folders of read-only studio shelves (`*.json`) |
| `ANIMTK_PICKER_PATH` | folders of studio pickers (`<RigAssetName>.json`), used before your own |
| `ANIMTK_POSE_LIBRARY_PATH` | extra pose libraries shown in the library combo |
| `ANIMTK_CHARACTER_PATH` | folders of studio character registries (`*.json`) |

Separate several folders with `;` on Windows (`:` elsewhere).

## Tests

```bash
python Plugins/AnimToolkit/Tests/python/test_animtk.py
```

These headless tests cover the parts that need no editor:
- the UI spec builder and event objects;
- the shelf model, runner and shipped shelves;
- pose blending and mirroring;
- the picker's auto layout;
- selection set resolution;
- the character registries;
- a compile check of every file.

`Tests/python/api_check.py` runs inside the editor as a commandlet, with no window. It checks that every `unreal` name
the toolkit uses exists in your engine build, builds the structs it constructs, and imports every module:

```bash
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "C:/Path/To/YourProject.uproject" -run=pythonscript -script="C:/Path/To/Plugins/AnimToolkit/Tests/python/api_check.py" -unattended -nullrhi
```

## Notes and limits

- **Blending and Animation mode:** the engine's blend tools only work while Animation mode (the Control Rig edit
  mode) is active, so `ops.blend` turns it on when needed. In UE 5.8,
  `ControlRigSequencerLibrary.blend_values_on_selected` returns `False` even when it blended, so `ops.blend` does not
  check its result. It raises only when nothing is selected.
- **Menus:** in UE 5.8 the level editor's menu bar is `MainFrame.MainMenu`. Entries added to `LevelEditor.MainMenu`
  do not appear, so the Anim Toolkit menu is added to `MainFrame.MainMenu`.
- **Runtime images:** icons and thumbnails loaded from files use `FSlateDynamicImageBrush`. A plain `FSlateImageBrush`
  draws white for any file that a style set did not register at start-up.

- **Mirroring** swaps left and right controls by name and mirrors centre controls across one plane. This suits rigs
  whose left and right controls have mirrored local axes, as the UE mannequin rigs do. Other rigs can change the
  rules with `pose.set_mirror_rules(...)`. The toolbelt's *Mirror* button uses the engine's own mirror instead.
- **Thumbnails** are viewport screenshots taken with `AutomationLibrary.take_high_res_screenshot`. They are written
  a moment after saving, and the library refreshes itself.
- **Engine selection sets:** UE 5.8 also has native selection sets (`ControlRigSequencerLibrary.get_selection_sets()`).
  The example tool keeps its own JSON so it is easy to read, share and learn from.
