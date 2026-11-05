# SPDX-License-Identifier: Apache-2.0
"""The example tools. Each module has a ``show()`` and a ``ui.Tool`` subclass; ALL lists them for menus and startup."""

# (module, class name, menu label, tooltip)
ALL = [
    ("animtk.shelf", "ShelfTool", "Anim Shelf", "Maya-style shelf of Python script buttons"),
    ("animtk.tools.picker", "PickerTool", "Rig Picker", "Click controls on a 2D layout of the rig"),
    ("animtk.tools.pose_library", "PoseLibraryTool", "Pose Library", "Save, apply, blend and mirror poses and clips"),
    ("animtk.tools.selection_sets", "SelectionSetsTool", "Selection Sets", "One-click groups of controls"),
    ("animtk.tools.toolbelt", "ToolbeltTool", "Anim Toolbelt", "Tween, key, timing and pose tools (AnimBot style)"),
    ("animtk.tools.character_loader", "CharacterLoaderTool", "Character Loader", "Load a character with its Control Rig, ready to animate"),
]


def tool_class(module_name, class_name):
    import importlib
    return getattr(importlib.import_module(module_name), class_name)
