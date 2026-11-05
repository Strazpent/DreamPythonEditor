# SPDX-License-Identifier: Apache-2.0
"""Animator operations on the selected Control Rig controls: blending (tween family), keys, timing, mirroring.

Every function works on the current Sequencer state and the current control selection, records one undo step and
raises ``AnimTKError`` with a message an animator can act on. Shelf buttons can call them directly::

    import animtk.ops as ops
    ops.key_selected()
"""

from . import core, pose, seq
from . import rig as rig_mod
from .core import AnimTKError

unreal = core.unreal

# Engine blend operations (UE 5.8 Animation Mode tools, exposed through ControlRigSequencerLibrary).
BLEND_MODES = [
    ("Tween", "TWEEN"),
    ("Blend to Neighbor", "BLEND_TO_NEIGHBOR"),
    ("Push / Pull", "PUSH_PULL"),
    ("Blend Relative", "BLEND_RELATIVE"),
    ("Blend to Ease", "BLEND_TO_EASE"),
    ("Smooth / Rough", "SMOOTH_ROUGH"),
]


# ------------------------------------------------------------------------------------------------------------- blend

ANIMATION_MODE = "EditMode.ControlRig"


def ensure_animation_mode():
    """The engine's blend tools only work while Animation mode (the Control Rig edit mode) is active."""
    if hasattr(unreal, "AnimTKLibrary") and not unreal.AnimTKLibrary.is_editor_mode_active(ANIMATION_MODE):
        unreal.AnimTKLibrary.activate_editor_mode(ANIMATION_MODE)


def blend(mode, value):
    """One engine blend of the selected controls at the playhead; ``value`` is -1..1 (beyond overshoots)."""
    sequence = seq.require()
    if not rig_mod.selected_controls() and not seq.selected_key_targets():
        raise AnimTKError("Nothing to blend: select controls (or keys in the Curve Editor)")
    ensure_animation_mode()
    operation = getattr(unreal.AnimToolBlendOperation, mode)
    # UE 5.8 returns False even when it blended (its DoBlend ignores the result), so the return value is not checked.
    unreal.ControlRigSequencerLibrary.blend_values_on_selected(sequence, operation, float(value))


class BlendDrag(object):
    """A live slider drag: each update starts again from the values captured at ``begin()``, so relative modes
    (Push/Pull, Blend Relative, Smooth/Rough) do not accumulate, and the whole drag is one undo step."""

    def __init__(self):
        self.interaction = core.Interaction("Blend")
        self.snapshot = []
        self.active = False

    def begin(self):
        selection = rig_mod.require_selection()
        self.interaction.begin()
        self.snapshot = [(r, pose.capture(r, names)) for r, names in rig_mod.by_rig(selection)]
        self.active = True

    def update(self, mode, value):
        if not self.active:
            self.begin()
        for r, clip in self.snapshot:
            pose.apply(r, clip, key=True)
        blend(mode, value)

    def end(self):
        self.active = False
        self.snapshot = []
        self.interaction.end()


# ------------------------------------------------------------------------------------------------------------- keys

def key_selected():
    selection = rig_mod.require_selection()
    with core.undo("Key selected controls"):
        for r, names in rig_mod.by_rig(selection):
            r.key(names)
    return len(selection)


def delete_keys_at_frame(frame=None):
    """Removes the selected controls' keys on ``frame`` (default: the playhead)."""
    frame = seq.current_frame() if frame is None else frame
    removed = 0
    with core.undo("Delete keys"):
        for r, names in rig_mod.by_rig(rig_mod.require_selection()):
            for channels in r.channels(names).values():
                for channel in channels:
                    for key in list(channel.get_keys()):
                        if int(key.get_time().frame_number.value) == frame:
                            channel.remove_key(key)
                            removed += 1
    seq.refresh()
    return removed


def _shift_key(key, delta):
    time = key.get_time()
    key.set_time(unreal.FrameNumber(int(time.frame_number.value) + delta), time.sub_frame)


def nudge(delta):
    """Moves the keys selected in Sequencer by ``delta`` frames; with none selected, moves the selected controls'
    keys on the playhead and the playhead with them (AnimBot style nudge)."""
    delta = int(delta)
    if delta == 0:
        return 0
    moved = 0
    with core.undo("Nudge keys"):
        targets = seq.selected_key_targets()
        if targets:
            for _channel, keys in targets:
                for key in sorted(keys, key=lambda k: k.get_time().frame_number.value, reverse=delta > 0):
                    _shift_key(key, delta)
                    moved += 1
        else:
            frame = seq.current_frame()
            for r, names in rig_mod.by_rig(rig_mod.require_selection()):
                for channels in r.channels(names).values():
                    for channel in channels:
                        for key in channel.get_keys():
                            if int(key.get_time().frame_number.value) == frame:
                                _shift_key(key, delta)
                                moved += 1
            seq.set_frame(frame + delta)
    seq.refresh()
    return moved


def share_keys():
    """Keys every selected control on every frame where any of them has a key."""
    selection = rig_mod.require_selection()
    grouped = rig_mod.by_rig(selection)
    times = set()
    for r, names in grouped:
        times.update(r.key_times(names))
    if not times:
        raise AnimTKError("The selected controls have no keys")
    with core.undo("Share keys"):
        for r, names in grouped:
            r.key(names, sorted(times))
    return len(times)


def bake(step=1, clean=True):
    """Keys the selected controls every ``step`` frames over the work range (selection range, else playback range);
    ``clean`` removes their keys between the steps ("bake on twos" with step=2)."""
    step = max(1, int(step))
    start, end = seq.work_range()
    keep = list(range(start, end + 1, step))
    keep_set = set(keep)
    with core.undo("Bake on {}s".format(step)):
        for r, names in rig_mod.by_rig(rig_mod.require_selection()):
            r.key(names, keep)
            if clean:
                for channels in r.channels(names).values():
                    for channel in channels:
                        for key in list(channel.get_keys()):
                            t = int(key.get_time().frame_number.value)
                            if start <= t <= end and t not in keep_set:
                                channel.remove_key(key)
    seq.refresh()
    return len(keep)


def stagger(step=1):
    """Offsets the keys of each selected control by ``step`` frames more than the previous one, in selection order."""
    step = int(step)
    with core.undo("Stagger keys"):
        index = 0
        for r, name in rig_mod.require_selection():
            offset = index * step
            index += 1
            if offset == 0:
                continue
            for channels in r.channels([name]).values():
                for channel in channels:
                    for key in sorted(channel.get_keys(), key=lambda k: k.get_time().frame_number.value, reverse=offset > 0):
                        _shift_key(key, offset)
    seq.refresh()


def jump_key(direction):
    """Moves the playhead to the next (+1) or previous (-1) key of the selected controls (all rig controls when none
    is selected)."""
    frame = seq.current_frame()
    times = set()
    selection = rig_mod.selected_controls()
    if selection:
        for r, names in rig_mod.by_rig(selection):
            times.update(r.key_times(names))
    else:
        for r in rig_mod.rigs():
            times.update(r.key_times())
    candidates = [t for t in times if (t > frame if direction > 0 else t < frame)]
    if not candidates:
        return frame
    target = min(candidates) if direction > 0 else max(candidates)
    seq.set_frame(target)
    return target


# ------------------------------------------------------------------------------------------------------------- pose

def reset_selected():
    """Puts the selected controls back to their initial values (and keys them)."""
    rig_mod.require_selection()
    with core.undo("Reset controls"):
        unreal.ControlRigSequencerLibrary.zero_transforms(True, True)


def mirror_selected():
    with core.undo("Mirror selected controls"):
        unreal.ControlRigSequencerLibrary.mirror_selected_controls()


def select_mirror(add=False):
    if add:
        unreal.ControlRigSequencerLibrary.add_mirrored_controls_to_selection()
    else:
        unreal.ControlRigSequencerLibrary.select_mirrored_controls()


def copy_pose():
    r = rig_mod.require_rig()
    return pose.copy(r)


def paste_pose(mirror=False):
    r = rig_mod.require_rig()
    return pose.paste(r, mirror=mirror, selected_only=bool(r.selected()))


# ------------------------------------------------------------------------------------------------------------- select

def select_all(rig=None):
    r = rig or rig_mod.require_rig()
    r.select(r.controls(), "replace")


def invert_selection(rig=None):
    r = rig or rig_mod.require_rig()
    current = set(r.selected())
    r.select([c for c in r.controls() if c not in current], "replace")


def select_children(rig=None):
    r = rig or rig_mod.require_rig()
    names = set()
    for name in r.selected():
        names.add(name)
        names.update(r.children(name))
    r.select(sorted(names), "replace")


def frame_selection():
    unreal.ControlRigSequencerLibrary.frame_selection()


def add_layer_from_selection():
    with core.undo("Add animation layer"):
        return unreal.ControlRigSequencerLibrary.add_anim_layer_from_selection()
