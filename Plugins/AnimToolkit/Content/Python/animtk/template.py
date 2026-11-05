# SPDX-License-Identifier: Apache-2.0
"""Starts a new tool from Resources/Templates/my_tool.py: copies it to Saved/AnimToolkit/Scripts, puts a button for it
on the shelf and opens it for editing (in the Dream Python Editor when installed)."""

import os

from . import core, shelf, ui


def new_tool(name=None):
    if name is None:
        ui.prompt("New Tool", "Tool name (letters, digits, underscores):", new_tool, "my_tool")
        return None
    module = "".join(c if c.isalnum() else "_" for c in name.strip().lower()).strip("_") or "my_tool"
    target = core.user_dir("Scripts", module + ".py")
    if os.path.exists(target):
        raise core.AnimTKError("{} already exists".format(target))
    with open(core.resource("Templates", "my_tool.py"), "r", encoding="utf-8") as handle:
        text = handle.read()
    title = module.replace("_", " ").title()
    text = text.replace("my_tool", module).replace("My Tool", title).replace("MyTool", title.replace(" ", ""))
    with open(target, "w", encoding="utf-8") as handle:
        handle.write(text)
    shelf.add_button(shelf.store().active or "Custom", title, script=target, tooltip="Opens " + title)
    shelf.open_script(target)
    core.notify("Created {}; its shelf button runs it".format(target), "success")
    return target


__all__ = ["new_tool"]
