# SPDX-License-Identifier: Apache-2.0
"""Poses and short animation clips of Control Rig controls, as JSON-friendly data.

A clip is a dict::

    {"type": "pose" | "anim", "name": "...", "rig": "/Game/.../CR_Mannequin_Body",
     "fps": 30.0, "length": 1, "controls": {"hand_l_ctrl": {"kind": "euler_transform", "values": [v0, v1, ...]}}}

A pose is a clip with one frame. The lerp and mirror helpers are pure Python (no editor needed).
"""

import datetime
import re

from . import core, seq

# --------------------------------------------------------------------------------------------------------- capture

def capture(rig, controls=None, start=None, end=None, name="", kind=None):
    """Reads the controls of ``rig`` (all animatable controls by default) at the playhead, or over start..end."""
    controls = list(controls) if controls else rig.controls()
    if start is None:
        start = end = seq.current_frame()
    if end is None:
        end = start
    frame_numbers = list(range(int(start), int(end) + 1))
    data = {}
    for name_ in controls:
        try:
            values = rig.get_many(name_, frame_numbers) if len(frame_numbers) > 1 else [rig.get(name_, frame_numbers[0])]
        except Exception as exc:  # noqa: BLE001 - one odd control must not lose the whole pose
            core.warn("Skipped {}: {}".format(name_, exc))
            continue
        data[name_] = {"kind": rig.kind(name_), "values": values}
    return {
        "type": kind or ("pose" if len(frame_numbers) == 1 else "anim"),
        "name": name,
        "rig": rig.asset_path,
        "fps": seq.display_rate(rig.sequence),
        "length": len(frame_numbers),
        "created": datetime.datetime.now().isoformat(timespec="seconds"),
        "controls": data,
    }


# --------------------------------------------------------------------------------------------------------- apply

def apply(rig, clip, frame=None, controls=None, weight=1.0, mirror=False, key=True, base=None):
    """Applies ``clip`` to ``rig`` at ``frame`` (default: the playhead).

    * ``controls`` limits it to those names (after mirroring).
    * ``weight`` < 1 blends from the current values (or from ``base``, a pose captured before a slider drag).
    * ``mirror`` swaps left and right names and mirrors centre controls (see ``mirror_clip``).
    * An anim clip keys every frame from ``frame`` on; ``weight`` and ``base`` apply to poses only.
    """
    frame = seq.current_frame() if frame is None else int(frame)
    if mirror:
        clip = mirror_clip(clip)
    wanted = set(controls) if controls else None
    available = set(rig.controls())
    applied = 0
    for name, entry in clip.get("controls", {}).items():
        if name not in available or (wanted is not None and name not in wanted):
            continue
        kind, values = entry["kind"], entry["values"]
        if not values:
            continue
        try:
            if len(values) > 1 and clip.get("type") == "anim":
                rig.set_many(name, list(range(frame, frame + len(values))), values)
            else:
                target = values[0]
                if weight < 0.999:
                    start_value = base["controls"][name]["values"][0] if base and name in base.get("controls", {}) else rig.get(name, frame)
                    target = lerp_value(kind, start_value, target, weight)
                rig.set(name, target, frame=frame, key=key)
            applied += 1
        except Exception as exc:  # noqa: BLE001
            core.warn("Could not apply {}: {}".format(name, exc))
    return applied


# --------------------------------------------------------------------------------------------------------- blending

def _lerp(a, b, t):
    return a + (b - a) * t


def _lerp_angle(a, b, t):
    """Shortest-path angle lerp in degrees."""
    delta = (b - a + 180.0) % 360.0 - 180.0
    return a + delta * t


def lerp_value(kind, a, b, t):
    """Blends two control values of ``kind``; t=0 gives a, t=1 gives b (t outside 0..1 extrapolates)."""
    if kind in ("float", "scale_float"):
        return _lerp(float(a), float(b), t)
    if kind == "integer":
        return int(round(_lerp(a, b, t)))
    if kind == "bool":
        return b if t >= 0.5 else a
    if kind in ("vector2d", "position", "scale"):
        return [_lerp(x, y, t) for x, y in zip(a, b)]
    if kind == "rotator":
        return [_lerp_angle(x, y, t) for x, y in zip(a, b)]
    return {
        "t": [_lerp(x, y, t) for x, y in zip(a["t"], b["t"])],
        "r": [_lerp_angle(x, y, t) for x, y in zip(a["r"], b["r"])],
        "s": [_lerp(x, y, t) for x, y in zip(a.get("s", [1, 1, 1]), b.get("s", [1, 1, 1]))],
    }


# --------------------------------------------------------------------------------------------------------- mirroring

# (left, right) name tokens, tried in order. Studios extend this with set_mirror_rules().
MIRROR_RULES = [
    ("_l_", "_r_"),
    ("_l", "_r"),      # suffix: hand_l -> hand_r
    ("l_", "r_"),      # prefix: l_hand -> r_hand
    ("_L_", "_R_"),
    ("_L", "_R"),
    ("L_", "R_"),
    ("Left", "Right"),
    ("left", "right"),
]

# Mirror plane for centre controls: translation axis to negate and rotation components (roll, pitch, yaw) to negate.
# The defaults mirror across X for a character facing +Y (the UE mannequin). Change with set_mirror_rules().
CENTER_FLIP = {"t": [True, False, False], "r": [False, True, True]}


def set_mirror_rules(rules=None, center_flip=None):
    global MIRROR_RULES, CENTER_FLIP
    if rules is not None:
        MIRROR_RULES = [tuple(r) for r in rules]
    if center_flip is not None:
        CENTER_FLIP = center_flip


def _swap(name, a, b):
    """``name`` with token ``a`` replaced by ``b``, or None when ``a`` is not a side token of ``name``."""
    if a.startswith("_") and a.endswith("_"):
        return name.replace(a, b, 1) if a in name else None
    if a.startswith("_"):
        if name.endswith(a):
            return name[: -len(a)] + b
        match = re.search(re.escape(a) + r"(?=_|\d|$)", name)
        return name[: match.start()] + b + name[match.end():] if match else None
    if a.endswith("_"):
        return b + name[len(a):] if name.startswith(a) else None
    return name.replace(a, b, 1) if a in name else None


def mirror_name(name):
    """The opposite side's name, or None for a centre control."""
    for left, right in MIRROR_RULES:
        for a, b in ((left, right), (right, left)):
            swapped = _swap(name, a, b)
            if swapped is not None:
                return swapped
    return None


def side_of(name):
    """"left", "right" or None (centre), from the same rules as mirror_name."""
    for left, right in MIRROR_RULES:
        if _swap(name, left, right) is not None:
            return "left"
        if _swap(name, right, left) is not None:
            return "right"
    return None


def mirror_value(kind, value):
    """Mirrors a centre control's value across the mirror plane (CENTER_FLIP)."""
    flip_t, flip_r = CENTER_FLIP["t"], CENTER_FLIP["r"]
    if kind == "position":
        return [-v if f else v for v, f in zip(value, flip_t)]
    if kind == "rotator":
        return [-v if f else v for v, f in zip(value, flip_r)]
    if isinstance(value, dict):
        return {
            "t": [-v if f else v for v, f in zip(value["t"], flip_t)],
            "r": [-v if f else v for v, f in zip(value["r"], flip_r)],
            "s": list(value.get("s", [1, 1, 1])),
        }
    return value


def mirror_clip(clip):
    """Left/right controls swap values; centre controls are mirrored. Works for rigs whose left and right controls
    have mirrored local axes (the UE mannequin rigs do); other rigs may need set_mirror_rules()."""
    mirrored = dict(clip)
    controls = {}
    for name, entry in clip.get("controls", {}).items():
        other = mirror_name(name)
        if other is None:
            controls[name] = {"kind": entry["kind"], "values": [mirror_value(entry["kind"], v) for v in entry["values"]]}
        else:
            controls[other] = {"kind": entry["kind"], "values": list(entry["values"])}
    mirrored["controls"] = controls
    return mirrored


# --------------------------------------------------------------------------------------------------------- clipboard

_clipboard = {}


def copy(rig, controls=None):
    """Keeps a pose of the selected (or given) controls for paste()."""
    controls = controls or rig.selected() or rig.controls()
    _clipboard["clip"] = capture(rig, controls, name="clipboard")
    _clipboard["rig"] = rig.name
    return len(_clipboard["clip"]["controls"])


def paste(rig, mirror=False, selected_only=True):
    clip = _clipboard.get("clip")
    if not clip:
        raise core.AnimTKError("Copy a pose first")
    limit = rig.selected() if selected_only else None
    if limit and mirror:
        # Selecting one side and pasting mirrored means "put it on the other side" too.
        limit = limit + [m for m in (mirror_name(n) for n in limit) if m]
    with core.undo("Paste pose"):
        return apply(rig, clip, controls=limit or None, mirror=mirror)


def has_clipboard():
    return bool(_clipboard.get("clip"))


def controls_of(clip):
    return list(clip.get("controls", {}).keys())


def is_compatible(clip, rig):
    """True when at least half of the clip's controls exist on ``rig``."""
    names = controls_of(clip)
    if not names:
        return False
    available = set(rig.controls())
    return sum(1 for n in names if n in available) * 2 >= len(names)


__all__ = ["capture", "apply", "lerp_value", "mirror_name", "side_of", "mirror_value", "mirror_clip", "copy", "paste",
           "set_mirror_rules", "is_compatible", "rig_mod"]
