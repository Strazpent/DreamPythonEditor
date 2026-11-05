# SPDX-License-Identifier: Apache-2.0
"""Editor start: lists the tools in Window > Tools > Anim Toolkit, adds an "Anim Toolkit" main menu and a toolbar
button that opens the shelf. ``init_unreal.py`` calls ``run()``; studios can call ``register_tool`` for their own."""

from . import core
from .tools import ALL, tool_class

unreal = core.unreal

# UE 5.8 shows MainFrame.MainMenu in the level editor; entries added to LevelEditor.MainMenu do not appear.
MAIN_MENUS = ("MainFrame.MainMenu", "LevelEditor.MainMenu")
OWNER = "AnimToolkit"


def register_tool(module_name, class_name, label, tooltip="", menu=None):
    """Lists a ui.Tool subclass in Window > Tools > Anim Toolkit and in the Anim Toolkit menu."""
    cls = tool_class(module_name, class_name)
    cls.register()
    menu = menu or _menu()
    if menu is None:
        return
    entry = unreal.ToolMenuEntry(name="AnimTK_" + class_name, type=unreal.MultiBlockType.MENU_ENTRY)
    entry.set_label(label)
    entry.set_tool_tip(tooltip)
    entry.set_string_command(unreal.ToolMenuStringCommandType.PYTHON, "", cls.factory_command())
    menu.add_menu_entry("Tools", entry)


def _menu():
    menus = unreal.ToolMenus.get()
    for main_name in MAIN_MENUS:
        main = menus.find_menu(main_name)
        if main is None:
            continue
        existing = menus.find_menu(main_name + ".AnimToolkit")
        if existing is not None:
            return existing
        menu = main.add_sub_menu(OWNER, "", "AnimToolkit", "Anim Toolkit", "Animation tools built with the Anim Toolkit Python API")
        menu.add_section("Tools", "Tools")
        menu.add_section("Develop", "Develop")
        return menu
    return None


def _add_develop_entries(menu):
    for name, label, tip, command in [
        ("AnimTK_Reload", "Reload Anim Toolkit", "Re-import every animtk module after editing a tool", "import animtk; animtk.reload()"),
        ("AnimTK_NewTool", "New Tool From Template...", "Copy the tool template to your shelf scripts and open it",
         "import animtk.template as t; t.new_tool()"),
        ("AnimTK_UserFolder", "Open Anim Toolkit Data Folder", "Shelves, pickers, poses and sets live here",
         "import animtk.core as c, animtk.shelf as s; s.open_folder(c.user_dir())"),
    ]:
        entry = unreal.ToolMenuEntry(name=name, type=unreal.MultiBlockType.MENU_ENTRY)
        entry.set_label(label)
        entry.set_tool_tip(tip)
        entry.set_string_command(unreal.ToolMenuStringCommandType.PYTHON, "", command)
        menu.add_menu_entry("Develop", entry)


def _add_toolbar_button():
    menus = unreal.ToolMenus.get()
    toolbar = menus.extend_menu("LevelEditor.LevelEditorToolBar.User")
    if toolbar is None:
        return
    entry = unreal.ToolMenuEntry(name="AnimTK_Shelf", type=unreal.MultiBlockType.TOOL_BAR_BUTTON)
    entry.set_label("Anim Shelf")
    entry.set_tool_tip("Open the Anim Toolkit shelf")
    entry.set_icon("EditorStyle", "Icons.Layout")
    entry.set_string_command(unreal.ToolMenuStringCommandType.PYTHON, "", tool_class("animtk.shelf", "ShelfTool").factory_command())
    toolbar.add_menu_entry("AnimToolkit", entry)


def run():
    if unreal is None or not hasattr(unreal, "AnimTKPanel"):
        core.warn("The AnimToolkit C++ module is not loaded; panels are unavailable")
        return
    menu = core.safe_call(_menu)
    for module_name, class_name, label, tooltip in ALL:
        core.safe_call(register_tool, module_name, class_name, label, tooltip, menu)
    if menu is not None:
        core.safe_call(_add_develop_entries, menu)
    core.safe_call(_add_toolbar_button)
    unreal.ToolMenus.get().refresh_all_widgets()
    core.log("Anim Toolkit ready: Window > Tools > Anim Toolkit, or the Anim Toolkit menu")
