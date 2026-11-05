# SPDX-License-Identifier: Apache-2.0
"""Anim Toolkit: build animation tools for Unreal in Python.

Modules:
    core   editor plumbing: log, notify, undo, paths, JSON, ticking
    seq    Sequencer: open sequence, playhead, ranges, bindings, keys
    rig    Control Rig in Sequencer: rigs, controls, selection, typed values, channels
    pose   poses and clips as JSON data: capture, apply, blend, mirror
    ops    animator operations: blend/tween, keys, nudge, bake, stagger, mirror
    ui     declarative Slate panels (ui.Tool, ui.VBox, ui.Button, ...)
    shelf  the Maya-style script shelf
    tools  the example tools (picker, pose library, selection sets, toolbelt, character loader)

``animtk.reload()`` re-imports everything after you edit a tool, without restarting the editor.
"""

__version__ = "0.1.0"


def reload():
    """Re-imports every animtk module (and reopens open tools with the new code)."""
    import importlib
    import sys

    from . import core
    names = sorted((n for n in sys.modules if n == "animtk" or n.startswith("animtk.")), key=lambda n: n.count("."), reverse=True)
    open_tools = []
    for name in names:
        module = sys.modules.get(name)
        for value in list(vars(module).values()) if module else []:
            inst = getattr(value, "__dict__", {}).get("_instance") if isinstance(value, type) else None
            if inst is not None and getattr(inst, "panel", None) is not None and inst.panel.is_open():
                open_tools.append((value.__module__, value.__name__))
    for name in names:
        if name != "animtk" and name in sys.modules:
            importlib.reload(sys.modules[name])
    from .tools import tool_class
    for module_name, class_name in set(open_tools):
        core.safe_call(lambda m=module_name, c=class_name: tool_class(m, c).show())
    core.notify("Anim Toolkit reloaded", "success")
