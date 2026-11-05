# SPDX-License-Identifier: Apache-2.0
"""Control Rig in Sequencer: rigs of the open sequence, control selection, typed values, keys and channels.

``Rig`` wraps one Control Rig track of the open Level Sequence. Values cross the boundary as plain Python data so
poses can be saved to JSON and blended:

=====================  =========================================================
control kind           Python value
=====================  =========================================================
float, scale_float     float
integer                int
bool                   bool
vector2d               [x, y]
position, scale        [x, y, z]
rotator                [roll, pitch, yaw]
euler_transform,       {"t": [x, y, z], "r": [roll, pitch, yaw], "s": [x, y, z]}
transform,
transform_no_scale
=====================  =========================================================
"""

import re

from . import core, seq
from .core import AnimTKError

unreal = core.unreal

KINDS = ("float", "scale_float", "integer", "bool", "vector2d", "position", "scale", "rotator", "euler_transform",
         "transform", "transform_no_scale")

# kind -> (single getter, plural getter, single setter, plural setter) on ControlRigSequencerLibrary
_ACCESSORS = {
    "float": ("get_local_control_rig_float", "get_local_control_rig_floats", "set_local_control_rig_float", "set_local_control_rig_floats"),
    "scale_float": ("get_local_control_rig_float", "get_local_control_rig_floats", "set_local_control_rig_float", "set_local_control_rig_floats"),
    "integer": ("get_local_control_rig_int", "get_local_control_rig_ints", "set_local_control_rig_int", "set_local_control_rig_ints"),
    "bool": ("get_local_control_rig_bool", "get_local_control_rig_bools", "set_local_control_rig_bool", "set_local_control_rig_bools"),
    "vector2d": ("get_local_control_rig_vector2d", "get_local_control_rig_vector2_ds", "set_local_control_rig_vector2d", "set_local_control_rig_vector2_ds"),
    "position": ("get_local_control_rig_position", "get_local_control_rig_positions", "set_local_control_rig_position", "set_local_control_rig_positions"),
    "scale": ("get_local_control_rig_scale", "get_local_control_rig_scales", "set_local_control_rig_scale", "set_local_control_rig_scales"),
    "rotator": ("get_local_control_rig_rotator", "get_local_control_rig_rotators", "set_local_control_rig_rotator", "set_local_control_rig_rotators"),
    "euler_transform": ("get_local_control_rig_euler_transform", "get_local_control_rig_euler_transforms", "set_local_control_rig_euler_transform", "set_local_control_rig_euler_transforms"),
    "transform": ("get_local_control_rig_transform", "get_local_control_rig_transforms", "set_local_control_rig_transform", "set_local_control_rig_transforms"),
    "transform_no_scale": ("get_local_control_rig_transform_no_scale", "get_local_control_rig_transform_no_scales", "set_local_control_rig_transform_no_scale", "set_local_control_rig_transform_no_scales"),
}


def _crsl():
    return unreal.ControlRigSequencerLibrary


def _enum_name(value):
    name = getattr(value, "name", None) or str(value)
    return name.split(".")[-1].lower()


_CPP_NAMES = {"transformnoscale": "transform_no_scale", "eulertransform": "euler_transform", "scalefloat": "scale_float",
              "vector2d": "vector2d"}


def _kind_from_text(text):
    """"(...ControlType=TransformNoScale,...)" -> "transform_no_scale"."""
    match = re.search(r"ControlType=([A-Za-z0-9]+)", text or "")
    if not match:
        return "euler_transform"
    raw = match.group(1).lower()
    return _CPP_NAMES.get(raw, raw)


def control_key(name):
    return unreal.RigElementKey(type=unreal.RigElementType.CONTROL, name=name)


# ------------------------------------------------------------------------------------------------------------- values

def _xyz(v):
    return [float(v.x), float(v.y), float(v.z)]


def _rot(r):
    return [float(r.roll), float(r.pitch), float(r.yaw)]


def to_python(kind, value):
    """An unreal value of a control of ``kind`` -> plain Python data (duck typed, so tests can pass fakes)."""
    if kind in ("float", "scale_float"):
        return float(value)
    if kind == "integer":
        return int(value)
    if kind == "bool":
        return bool(value)
    if kind == "vector2d":
        return [float(value.x), float(value.y)]
    if kind in ("position", "scale"):
        return _xyz(value)
    if kind == "rotator":
        return _rot(value)
    if kind == "euler_transform":
        return {"t": _xyz(value.location), "r": _rot(value.rotation), "s": _xyz(value.scale)}
    if kind in ("transform", "transform_no_scale"):
        rotation = value.rotation.rotator() if hasattr(value.rotation, "rotator") else value.rotation
        scale = getattr(value, "scale3d", None)
        return {"t": _xyz(value.translation if hasattr(value, "translation") else value.location),
                "r": _rot(rotation),
                "s": _xyz(scale) if scale is not None else [1.0, 1.0, 1.0]}
    raise AnimTKError("Unknown control kind: {}".format(kind))


def to_unreal(kind, value):
    """Plain Python data -> the unreal value a setter of ``kind`` takes."""
    if kind in ("float", "scale_float"):
        return float(value)
    if kind == "integer":
        return int(round(value))
    if kind == "bool":
        return bool(value)
    if kind == "vector2d":
        return unreal.Vector2D(value[0], value[1])
    if kind in ("position", "scale"):
        return unreal.Vector(value[0], value[1], value[2])
    if kind == "rotator":
        return unreal.Rotator(roll=value[0], pitch=value[1], yaw=value[2])
    t, r, s = value["t"], value["r"], value.get("s", [1.0, 1.0, 1.0])
    location = unreal.Vector(t[0], t[1], t[2])
    rotator = unreal.Rotator(roll=r[0], pitch=r[1], yaw=r[2])
    scale = unreal.Vector(s[0], s[1], s[2])
    if kind == "euler_transform":
        return unreal.EulerTransform(location=location, rotation=rotator, scale=scale)
    if kind == "transform":
        return unreal.Transform(location=location, rotation=rotator, scale=scale)
    if kind == "transform_no_scale":
        result = unreal.TransformNoScale()
        result.location = location
        result.rotation = rotator.quaternion()
        return result
    raise AnimTKError("Unknown control kind: {}".format(kind))


# ------------------------------------------------------------------------------------------------------------- rig

class Rig(object):
    """One Control Rig track in the open Level Sequence."""

    def __init__(self, proxy, sequence):
        self.proxy = proxy
        self.sequence = sequence
        self.control_rig = proxy.control_rig
        self.track = proxy.track
        self.binding = proxy.proxy
        self._kinds = {}
        self._controls = {}

    def __repr__(self):
        return "Rig({!r})".format(self.name)

    def __eq__(self, other):
        return isinstance(other, Rig) and other.control_rig == self.control_rig

    def __hash__(self):
        return hash(self.name)

    # Identity ------------------------------------------------------------------------------------------------------
    @property
    def name(self):
        """The binding's display name (the actor label in the Sequencer outliner)."""
        try:
            return str(self.binding.get_display_name())
        except Exception:  # noqa: BLE001
            return self.control_rig.get_name()

    @property
    def asset_path(self):
        """The Control Rig asset this track runs ("/Game/.../CR_Mannequin_Body"), best effort."""
        cls = self.control_rig.get_class()
        path = cls.get_path_name()
        # "/Game/Rigs/CR_X.CR_X_C" -> "/Game/Rigs/CR_X"
        return path.rsplit(".", 1)[0] if "." in path else path

    @property
    def hierarchy(self):
        return self.control_rig.get_hierarchy()

    def actors(self):
        return [o for o in seq.bound_objects(self.binding) if isinstance(o, unreal.Actor)]

    def section(self):
        """The section new keys go to."""
        section = self.track.get_section_to_key()
        if section is None:
            sections = self.track.get_sections()
            section = sections[0] if sections else None
        if section is None:
            raise AnimTKError("{} has no Control Rig section to key".format(self.name))
        return section

    # Controls ------------------------------------------------------------------------------------------------------
    def controls(self, animatable_only=True):
        """Control names in hierarchy order (cached on this Rig object; rig.rigs() hands out fresh ones)."""
        cached = self._controls.get(animatable_only)
        if cached is not None:
            return list(cached)
        names = []
        hierarchy = self.hierarchy
        for key in hierarchy.get_controls(True):
            if animatable_only:
                settings = hierarchy.get_control_settings(key)
                # ANIMATION_CONTROL and ANIMATION_CHANNEL are keyable; proxies and visual cues are not.
                if _enum_name(settings.animation_type) not in ("animation_control", "animation_channel"):
                    continue
            names.append(str(key.name))
        self._controls[animatable_only] = names
        return list(names)

    def has_control(self, name):
        return self.hierarchy.contains(control_key(name))

    def kind(self, name):
        """The control's value kind (see the module docstring)."""
        cached = self._kinds.get(name)
        if cached is None:
            settings = self.hierarchy.get_control_settings(control_key(name))
            try:
                cached = _enum_name(settings.control_type)
            except TypeError:
                # Transform and TransformNoScale are hidden from Python, so reading them raises: use the exported text.
                cached = _kind_from_text(settings.export_text())
            if cached not in _ACCESSORS:
                cached = "euler_transform"
            self._kinds[name] = cached
        return cached

    def settings(self, name):
        return self.hierarchy.get_control_settings(control_key(name))

    def color(self, name):
        color = self.settings(name).shape_color
        return [color.r, color.g, color.b, 1.0]

    def initial_global_position(self, name):
        """The control's rest position in rig space (what the picker's auto layout projects)."""
        transform = self.hierarchy.get_global_transform(control_key(name), True)
        return _xyz(transform.translation)

    def parent(self, name):
        parents = self.hierarchy.get_parents(control_key(name), False)
        return str(parents[0].name) if parents else None

    def children(self, name, recursive=True, controls_only=True):
        keys = self.hierarchy.get_children(control_key(name), recursive)
        return [str(k.name) for k in keys if not controls_only or k.type == unreal.RigElementType.CONTROL]

    # Selection -----------------------------------------------------------------------------------------------------
    def selected(self):
        return [str(n) for n in self.control_rig.current_control_selection()]

    def select(self, names, mode="replace"):
        """``mode``: "replace", "add", "remove" or "toggle"."""
        rig = self.control_rig
        names = [n for n in names if n]
        if mode == "replace":
            rig.clear_control_selection()
            for name in names:
                rig.select_control(name, True)
        elif mode == "add":
            for name in names:
                rig.select_control(name, True)
        elif mode == "remove":
            for name in names:
                rig.select_control(name, False)
        elif mode == "toggle":
            for name in names:
                rig.select_control(name, not rig.is_control_selected(name))
        else:
            raise AnimTKError("Unknown selection mode {}".format(mode))

    def clear_selection(self):
        self.control_rig.clear_control_selection()

    # Values --------------------------------------------------------------------------------------------------------
    def get(self, name, frame=None):
        """The control's local value at ``frame`` (default: the playhead) as Python data."""
        kind = self.kind(name)
        frame = seq.current_frame() if frame is None else frame
        getter = getattr(_crsl(), _ACCESSORS[kind][0])
        return to_python(kind, getter(self.sequence, self.control_rig, name, unreal.FrameNumber(int(frame))))

    def get_many(self, name, frame_numbers):
        kind = self.kind(name)
        getter = getattr(_crsl(), _ACCESSORS[kind][1])
        values = getter(self.sequence, self.control_rig, name, seq.frames(frame_numbers))
        return [to_python(kind, v) for v in values]

    def set(self, name, value, frame=None, key=True):
        """Sets the control's local value at ``frame``; ``key=False`` changes the value without a key."""
        kind = self.kind(name)
        frame = seq.current_frame() if frame is None else frame
        setter = getattr(_crsl(), _ACCESSORS[kind][2])
        setter(self.sequence, self.control_rig, name, unreal.FrameNumber(int(frame)), to_unreal(kind, value), set_key=key)

    def set_many(self, name, frame_numbers, values):
        """Keys ``values`` (one per frame) in one call."""
        kind = self.kind(name)
        setter = getattr(_crsl(), _ACCESSORS[kind][3])
        setter(self.sequence, self.control_rig, name, seq.frames(frame_numbers), [to_unreal(kind, v) for v in values])

    def world_transform(self, name, frame=None):
        frame = seq.current_frame() if frame is None else frame
        return _crsl().get_control_rig_world_transform(self.sequence, self.control_rig, name, unreal.FrameNumber(int(frame)))

    def set_world_transform(self, name, transform, frame=None, key=True):
        frame = seq.current_frame() if frame is None else frame
        _crsl().set_control_rig_world_transform(self.sequence, self.control_rig, name, unreal.FrameNumber(int(frame)), transform, set_key=key)

    # Keys ----------------------------------------------------------------------------------------------------------
    def key(self, names=None, frame_numbers=None):
        """Keys the controls' current values (all selected controls by default) now or at the given frames."""
        names = list(names) if names is not None else self.selected()
        if not names:
            return
        section = self.section()
        if frame_numbers:
            _crsl().key_controls_at_frames(section, names, seq.frames(frame_numbers))
        else:
            _crsl().key_controls(section, names)

    def channels(self, names=None):
        """{control: [scripting channel, ...]} for the given controls (all animatable controls by default).

        Control Rig channels are named "<control>" (floats, ints, bools) or "<control>.<Group>.<Axis>" such as
        "hand_l_ctrl.Location.X".
        """
        wanted = set(names) if names is not None else None
        result = {}
        for section in self.track.get_sections():
            for channel in section.get_all_channels():
                channel_name = str(channel.channel_name)
                control = channel_name.split(".", 1)[0]
                if wanted is not None and control not in wanted:
                    continue
                result.setdefault(control, []).append(channel)
        return result

    def key_times(self, names=None):
        times = set()
        for channels in self.channels(names).values():
            for channel in channels:
                times.update(seq.key_times(channel))
        return sorted(times)


# ------------------------------------------------------------------------------------------------------------- lookup

def rigs(sequence=None):
    """Every Control Rig track of the open sequence."""
    sequence = sequence or seq.current()
    if sequence is None:
        return []
    return [Rig(proxy, sequence) for proxy in _crsl().get_control_rigs(sequence) if proxy.control_rig is not None]


def find_rig(name, sequence=None):
    for rig in rigs(sequence):
        if rig.name == name:
            return rig
    return None


def active_rigs(sequence=None):
    """Rigs with selected controls; all rigs when none has a selection."""
    all_rigs = rigs(sequence)
    with_selection = [r for r in all_rigs if r.selected()]
    return with_selection or all_rigs


def require_rig(sequence=None):
    """The first rig with selected controls, else the only rig; raises when that is ambiguous or empty."""
    all_rigs = rigs(sequence)
    if not all_rigs:
        raise AnimTKError("The open sequence has no Control Rig track")
    for rig in all_rigs:
        if rig.selected():
            return rig
    if len(all_rigs) == 1:
        return all_rigs[0]
    raise AnimTKError("Select a control so I know which rig to use")


def selected_controls(sequence=None):
    """[(rig, control name)] for every selected control of every rig."""
    result = []
    for rig in rigs(sequence):
        for name in rig.selected():
            result.append((rig, name))
    return result


def require_selection(sequence=None):
    selection = selected_controls(sequence)
    if not selection:
        raise AnimTKError("Select one or more controls first")
    return selection


def by_rig(pairs):
    """[(rig, control)] -> [(rig, [controls])], keeping first-seen order."""
    order = []
    grouped = {}
    for rig, name in pairs:
        if rig.name not in grouped:
            grouped[rig.name] = (rig, [])
            order.append(rig.name)
        grouped[rig.name][1].append(name)
    return [grouped[name] for name in order]
