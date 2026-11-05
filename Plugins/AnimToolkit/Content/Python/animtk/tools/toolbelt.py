# SPDX-License-Identifier: Apache-2.0
"""Anim Toolbelt: a compact strip of everyday animator tools, in the spirit of Maya's AnimBot.

* Blend slider with six modes (Tween, Blend to Neighbor, Push/Pull, Blend Relative, Blend to Ease, Smooth/Rough)
  and one-click presets; a drag is one undo step and never accumulates. "Overshoot" widens the range to +-150%.
* Keys: key, delete, nudge left/right, share keys, bake on ones/twos/threes, stagger.
* Timeline: previous / next key of the selected controls.
* Pose: copy, paste, paste mirrored, reset, mirror; selection: all, invert, children, mirror, frame; add a layer.

Every button calls a function in ``animtk.ops``; read that module to see how each tool is built, or to call the same
functions from a shelf button.
"""

from .. import core, ops, seq, ui

PRESETS = [-1.0, -0.75, -0.5, -0.25, 0.0, 0.25, 0.5, 0.75, 1.0]


class ToolbeltTool(ui.Tool):
    panel_id = "animtk_toolbelt"
    title = "Anim Toolbelt"

    def __init__(self):
        super(ToolbeltTool, self).__init__()
        self.mode_index = 0
        self.overshoot = False
        self.drag = ops.BlendDrag()

    @property
    def mode(self):
        return ops.BLEND_MODES[self.mode_index][1]

    def build(self):
        limit = 1.5 if self.overshoot else 1.0
        preset_values = PRESETS + ([-1.5, 1.5] if self.overshoot else [])
        presets = [ui.Button("{:+d}".format(int(v * 100)) if v else "0", flat=True, style="small", min_width=30,
                             tooltip="Blend {:+d}%".format(int(v * 100)),
                             on_clicked=lambda e, v=v: self._preset(v))
                   for v in sorted(preset_values)]

        def btn(text, fn, tip, icon=None):
            return ui.Button(text, icon=icon, tooltip=tip, style="small", on_clicked=lambda e: self._do(fn))

        return ui.Scroll(
            ui.Section("Blend",
                ui.HBox(
                    ui.Combo([label for label, _m in ops.BLEND_MODES], value=ops.BLEND_MODES[self.mode_index][0], id="mode", on_changed=self._mode_changed),
                    ui.Toggle("Overshoot", value=self.overshoot, tooltip="Allow -150% to +150%", on_changed=self._overshoot),
                ),
                ui.HBox(
                    ui.Label("", id="readout", min_width=40),
                    ui.Slider(0.0, -limit, limit, id="slider", on_begin=self._begin, on_changed=self._changed, on_end=self._end,
                              tooltip="Drag to blend the selected controls at the playhead"),
                ),
                ui.HBox(*presets, padding=0),
            ),
            ui.Section("Keys",
                ui.Wrap(
                    btn("Key", ops.key_selected, "Key the selected controls"),
                    btn("Delete", ops.delete_keys_at_frame, "Delete the selected controls' keys on this frame"),
                    btn("< Nudge", lambda: ops.nudge(-self._step()), "Move keys left (selected keys, else keys on this frame)"),
                    btn("Nudge >", lambda: ops.nudge(self._step()), "Move keys right"),
                    btn("Share", ops.share_keys, "Key every selected control wherever any of them has a key"),
                    btn("Bake 1s", lambda: ops.bake(1), "Key every frame of the work range"),
                    btn("Bake 2s", lambda: ops.bake(2), "Key on twos and remove the in-betweens"),
                    btn("Bake 3s", lambda: ops.bake(3), "Key on threes and remove the in-betweens"),
                    btn("Stagger", lambda: ops.stagger(self._step()), "Offset each selected control by one more step"),
                    spacing=2,
                ),
                ui.HBox(ui.Label("Step"), ui.Spin(1, 1, 24, integer=True, id="step", width=60), ui.Spacer(stretch=1)),
            ),
            ui.Section("Timeline",
                ui.HBox(
                    btn("|< Prev Key", lambda: ops.jump_key(-1), "Previous key of the selected controls"),
                    btn("< Frame", lambda: seq.set_frame(seq.current_frame() - 1), "One frame back"),
                    btn("Frame >", lambda: seq.set_frame(seq.current_frame() + 1), "One frame forward"),
                    btn("Next Key >|", lambda: ops.jump_key(1), "Next key of the selected controls"),
                ),
            ),
            ui.Section("Pose",
                ui.Wrap(
                    btn("Copy", self._copy, "Copy the selected controls' pose"),
                    btn("Paste", lambda: ops.paste_pose(False), "Paste onto the selected controls"),
                    btn("Paste Mirror", lambda: ops.paste_pose(True), "Paste onto the opposite side"),
                    btn("Reset", ops.reset_selected, "Reset the selected controls"),
                    btn("Mirror", ops.mirror_selected, "Mirror the selected controls' pose"),
                    spacing=2,
                ),
            ),
            ui.Section("Selection",
                ui.Wrap(
                    btn("All", ops.select_all, "Select every control of the rig"),
                    btn("Invert", ops.invert_selection, "Invert the control selection"),
                    btn("Children", ops.select_children, "Add every control under the selected ones"),
                    btn("Mirror", lambda: ops.select_mirror(core.modifiers() == {"shift"}), "Select the mirrored controls (Shift: add)"),
                    btn("Frame", ops.frame_selection, "Frame the selection in the viewport"),
                    btn("+ Layer", ops.add_layer_from_selection, "Add an animation layer from the selection"),
                    spacing=2,
                ),
                collapsed=True,
            ),
            ui.Label("", id="status", style="small", color=[0.6, 0.6, 0.6, 1]),
        )

    # Helpers ---------------------------------------------------------------------------------------------------------
    def _step(self):
        return int(self.get("step") or 1)

    def _do(self, fn):
        result = fn()
        if isinstance(result, int):
            self.set("status", "text", "Done ({})".format(result))

    def _copy(self):
        count = ops.copy_pose()
        self.set("status", "text", "Copied {} controls".format(count))

    # Blend -----------------------------------------------------------------------------------------------------------
    def _mode_changed(self, event):
        self.mode_index = event.index or 0

    def _overshoot(self, event):
        self.overshoot = bool(event.value)
        self.rebuild()

    def _begin(self, event):
        core.safe_call(self.drag.begin)

    def _changed(self, event):
        value = float(event.value)
        self.set("readout", "text", "{:+d}%".format(int(round(value * 100))))
        if self.drag.active:
            self.drag.update(self.mode, value)

    def _end(self, event):
        self.drag.end()
        self.set("slider", "value", 0.0)
        self.set("readout", "text", "")

    def _preset(self, value):
        with core.undo("Blend {:+d}%".format(int(value * 100))):
            ops.blend(self.mode, value)


def show():
    return ToolbeltTool.show()
