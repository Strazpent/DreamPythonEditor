# SPDX-License-Identifier: Apache-2.0
"""Sequencer: the open Level Sequence, the playhead, ranges, bindings and keys.

Frames are display-rate frame numbers (what the Sequencer timeline shows) everywhere in animtk.
"""

from . import core
from .core import AnimTKError

unreal = core.unreal


def _lse():
    return unreal.LevelSequenceEditorBlueprintLibrary


def _subsystem():
    return unreal.get_editor_subsystem(unreal.LevelSequenceEditorSubsystem)


# ------------------------------------------------------------------------------------------------------------- sequence

def current():
    """The Level Sequence open in Sequencer (the focused sub-sequence when one is focused), or None."""
    lse = _lse()
    sequence = lse.get_focused_level_sequence()
    return sequence if sequence is not None else lse.get_current_level_sequence()


def require():
    sequence = current()
    if sequence is None:
        raise AnimTKError("Open a Level Sequence in Sequencer first")
    return sequence


def open_sequence(sequence):
    if isinstance(sequence, str):
        sequence = core.load_asset(sequence)
    _lse().open_level_sequence(sequence)
    return sequence


def create_sequence(asset_path):
    """Creates a Level Sequence asset ("/Game/Cinematics/Shot010") and returns it."""
    package_path, name = asset_path.rsplit("/", 1)
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    sequence = tools.create_asset(name, package_path, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
    if sequence is None:
        raise AnimTKError("Could not create the Level Sequence {}".format(asset_path))
    return sequence


def ensure_sequence(default_path="/Game/Cinematics/AnimTK_Scratch"):
    """The open sequence; otherwise opens (creating when needed) ``default_path``."""
    sequence = current()
    if sequence is not None:
        return sequence
    if core.asset_exists(default_path):
        sequence = core.load_asset(default_path)
    else:
        sequence = create_sequence(default_path)
        core.notify("Created {}".format(default_path))
    open_sequence(sequence)
    return sequence


def refresh():
    _lse().refresh_current_level_sequence()


# ------------------------------------------------------------------------------------------------------------- time

def current_frame():
    """The playhead, in display-rate frames of the open (focused) sequence."""
    lse = _lse()
    try:
        return int(lse.get_local_position().frame.frame_number.value)
    except AttributeError:
        return int(lse.get_current_local_time())


def set_frame(frame):
    lse = _lse()
    try:
        params = unreal.MovieSceneSequencePlaybackParams()
        params.frame = unreal.FrameTime(frame_number=unreal.FrameNumber(int(frame)))
        params.position_type = unreal.MovieScenePositionType.FRAME
        params.update_method = unreal.UpdatePositionMethod.SCRUB
        lse.set_local_position(params)
    except AttributeError:
        lse.set_current_local_time(int(frame))


def playback_range(sequence=None):
    """(start, end) inclusive, display-rate frames."""
    sequence = sequence or require()
    return int(sequence.get_playback_start()), int(sequence.get_playback_end()) - 1


def selection_range():
    """(start, end) of the Sequencer selection range, or None when there is none."""
    lse = _lse()
    start, end = lse.get_selection_range_start(), lse.get_selection_range_end()
    if end <= start:
        return None
    return int(start), int(end) - 1


def work_range(sequence=None):
    """The selection range when set, else the playback range: what "bake" style tools act on."""
    return selection_range() or playback_range(sequence)


def display_rate(sequence=None):
    sequence = sequence or require()
    rate = sequence.get_display_rate()
    return float(rate.numerator) / float(rate.denominator or 1)


def frames(numbers):
    """Python ints -> the FrameNumber array Sequencer functions expect."""
    return [unreal.FrameNumber(int(n)) for n in numbers]


def is_playing():
    return _lse().is_playing()


def play():
    _lse().play()


def pause():
    _lse().pause()


# ------------------------------------------------------------------------------------------------------------- bindings

def bindings(sequence=None):
    sequence = sequence or require()
    return list(sequence.get_bindings())


def find_binding(name, sequence=None):
    sequence = sequence or require()
    binding = sequence.find_binding_by_name(name)
    return binding if binding.is_valid() else None


def bound_objects(binding):
    return list(_lse().get_bound_objects(binding))


def binding_for_actor(actor, sequence=None):
    for binding in bindings(sequence):
        if actor in bound_objects(binding):
            return binding
    return None


def add_actor(actor, spawnable=False):
    """Adds ``actor`` to the open sequence and returns its binding (converted to a spawnable when asked)."""
    require()
    added = _subsystem().add_actors([actor])
    if not added:
        raise AnimTKError("Sequencer did not add {}".format(actor.get_actor_label()))
    binding = added[0]
    if spawnable:
        converted = _subsystem().convert_to_spawnable(binding)
        if converted:
            binding = converted[0]
    return binding


def remove_binding(binding):
    binding.remove()
    refresh()


def selected_bindings():
    return list(_lse().get_selected_bindings())


# ------------------------------------------------------------------------------------------------------------- keys

def key_times(channel):
    """Sorted display-rate frames of the keys on one scripting channel."""
    times = set()
    for key in channel.get_keys():
        times.add(int(key.get_time().frame_number.value))
    return sorted(times)


def selected_key_targets():
    """[(channel, [key, ...])] for the keys selected in the Sequencer or Curve Editor."""
    lse = _lse()
    result = []
    for proxy in lse.get_channels_with_selected_keys():
        channel = proxy.section.get_channel(str(proxy.channel_name))
        if channel is None:
            continue
        indices = list(lse.get_selected_keys(proxy))
        if not indices:
            continue
        result.append((channel, list(channel.get_keys_by_index(indices))))
    return result
