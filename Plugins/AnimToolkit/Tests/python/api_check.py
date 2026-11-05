# SPDX-License-Identifier: Apache-2.0
"""Headless API check, run inside the editor as a commandlet (no window):

    UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script=<this file> -unattended -nullrhi

Checks that every ``unreal.X.Y`` name the toolkit's Python uses exists in this engine build, that the structs it
builds construct, and that every animtk module imports. Writes a JSON report (ANIMTK_CHECK_OUT, default
<Project>/Saved/AnimToolkit/api_check.json) and logs "ANIMTK_API_CHECK PASS" or "ANIMTK_API_CHECK FAIL".
"""

import json
import os
import re
import sys
import traceback

import unreal

PLUGIN = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT = os.environ.get("ANIMTK_CHECK_OUT") or os.path.join(
    unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir()), "AnimToolkit", "api_check.json")

REFERENCE = re.compile(r"\bunreal\.([A-Z][A-Za-z0-9_]*)(\.[A-Za-z_][A-Za-z0-9_]*)?")

# Members reached through helper functions (so the regex above cannot see them).
MEMBERS = {
    "ControlRigSequencerLibrary": [
        "get_control_rig_world_transform", "get_control_rigs", "key_controls", "key_controls_at_frames",
        "set_control_rig_world_transform", "blend_values_on_selected", "zero_transforms", "mirror_selected_controls",
        "select_mirrored_controls", "add_mirrored_controls_to_selection", "frame_selection",
        "add_anim_layer_from_selection", "find_or_create_control_rig_track"],
    "LevelSequenceEditorBlueprintLibrary": [
        "get_bound_objects", "get_selected_bindings", "is_playing", "open_level_sequence", "pause", "play",
        "refresh_current_level_sequence", "get_channels_with_selected_keys", "get_current_level_sequence",
        "get_current_local_time", "get_focused_level_sequence", "get_local_position", "get_selected_keys",
        "get_selection_range_end", "get_selection_range_start", "set_current_local_time", "set_local_position"],
    "LevelSequenceEditorSubsystem": ["add_actors", "convert_to_spawnable"],
    "EditorActorSubsystem": ["destroy_actor", "get_all_level_actors", "get_selected_level_actors",
                             "set_selected_level_actors", "spawn_actor_from_object"],
    "AnimTKPanel": ["find_or_create_panel", "find_panel", "get_panel_ids", "register_tab", "set_spec", "show", "close",
                    "is_open", "set_prop", "get_value", "set_title", "get_panel_id", "destroy"],
    "AnimTKLibrary": ["notify", "get_modifier_keys", "clipboard_copy", "clipboard_paste", "activate_editor_mode",
                      "is_editor_mode_active"],
    "ControlRig": ["current_control_selection", "select_control", "clear_control_selection", "is_control_selected",
                   "get_hierarchy"],
    "RigHierarchy": ["get_controls", "get_control_settings", "contains", "get_global_transform", "get_parents",
                     "get_children"],
    "MovieSceneTrack": ["get_section_to_key", "get_sections"],
    "MovieSceneSection": ["get_all_channels", "get_channel"],
    "MovieSceneScriptingFloatChannel": ["get_keys", "remove_key", "get_keys_by_index"],
    "MovieSceneScriptingFloatKey": ["get_time", "set_time"],
    "MovieSceneBindingProxy": ["get_display_name", "get_tracks", "remove", "remove_track", "is_valid"],
    "LevelSequence": ["get_bindings", "find_binding_by_name", "get_playback_start", "get_playback_end",
                      "set_playback_start", "set_playback_end", "get_display_rate"],
    "ControlRigBlueprintLibrary": ["get_preview_mesh"],
    "Blueprint": ["generated_class"],
    "AutomationLibrary": ["take_high_res_screenshot"],
    "SystemLibrary": ["begin_transaction", "end_transaction"],
    "ToolMenus": ["get", "find_menu", "register_menu", "extend_menu", "refresh_all_widgets"],
    "ToolMenu": ["add_sub_menu", "add_section", "add_menu_entry"],
    "ToolMenuEntry": ["set_label", "set_tool_tip", "set_string_command", "set_icon"],
    "AssetRegistry": ["get_assets_by_class"],
    "EditorAssetLibrary": ["load_asset", "does_asset_exist"],
    "AssetEditorSubsystem": ["open_editor_for_assets"],
    "AssetTools": ["create_asset"],
    "EditorDialog": ["show_message"],
}
for _kind in ("float", "int", "bool", "vector2d", "position", "scale", "rotator", "euler_transform", "transform",
              "transform_no_scale"):
    MEMBERS["ControlRigSequencerLibrary"] += ["get_local_control_rig_" + _kind, "set_local_control_rig_" + _kind]
for _plural in ("floats", "ints", "bools", "vector2_ds", "positions", "scales", "rotators", "euler_transforms",
                "transforms", "transform_no_scales"):
    MEMBERS["ControlRigSequencerLibrary"] += ["get_local_control_rig_" + _plural, "set_local_control_rig_" + _plural]

ENUMS = {
    "AnimToolBlendOperation": ["TWEEN", "BLEND_TO_NEIGHBOR", "PUSH_PULL", "BLEND_RELATIVE", "BLEND_TO_EASE", "SMOOTH_ROUGH"],
    "MovieScenePositionType": ["FRAME"],
    "UpdatePositionMethod": ["SCRUB"],
    "ToolMenuStringCommandType": ["PYTHON"],
    "MultiBlockType": ["MENU_ENTRY", "TOOL_BAR_BUTTON"],
    "MultiBoxType": ["MENU"],
    "AppMsgType": ["YES_NO", "OK"],
    "AppReturnType": ["YES"],
    "RigElementType": ["CONTROL"],
}

STRUCTS = {
    "FrameTime": lambda: unreal.FrameTime(frame_number=unreal.FrameNumber(3)),
    "Rotator": lambda: unreal.Rotator(roll=1, pitch=2, yaw=3),
    "EulerTransform": lambda: unreal.EulerTransform(location=unreal.Vector(1, 2, 3), rotation=unreal.Rotator(roll=0, pitch=0, yaw=0), scale=unreal.Vector(1, 1, 1)),
    "Transform": lambda: unreal.Transform(location=unreal.Vector(1, 2, 3), rotation=unreal.Rotator(roll=0, pitch=0, yaw=0), scale=unreal.Vector(1, 1, 1)),
    "TransformNoScale": lambda: unreal.TransformNoScale(),
    "RigElementKey": lambda: unreal.RigElementKey(type=unreal.RigElementType.CONTROL, name="x"),
    "TopLevelAssetPath": lambda: unreal.TopLevelAssetPath("/Script/ControlRigDeveloper", "ControlRigBlueprint"),
    "ToolMenuEntry": lambda: unreal.ToolMenuEntry(name="x", type=unreal.MultiBlockType.MENU_ENTRY),
    "MovieSceneSequencePlaybackParams": lambda: unreal.MovieSceneSequencePlaybackParams(),
}

MODULES = ["animtk", "animtk.core", "animtk.seq", "animtk.rig", "animtk.pose", "animtk.ops", "animtk.ui", "animtk.shelf",
           "animtk.startup", "animtk.template", "animtk.tools.picker", "animtk.tools.pose_library",
           "animtk.tools.selection_sets", "animtk.tools.toolbelt", "animtk.tools.character_loader"]


def source_references():
    refs = set()
    for folder in ("Content", "Resources"):
        for dirpath, _dirs, files in os.walk(os.path.join(PLUGIN, folder)):
            for name in files:
                if name.endswith(".py"):
                    with open(os.path.join(dirpath, name), encoding="utf-8") as handle:
                        for match in REFERENCE.finditer(handle.read()):
                            refs.add("unreal." + match.group(1) + (match.group(2) or ""))
    return sorted(refs)


def main():
    refs = source_references()
    missing = []
    for ref in refs:
        obj = unreal
        for part in ref.split(".")[1:]:
            if not hasattr(obj, part):
                missing.append(ref)
                break
            obj = getattr(obj, part)
    for cls_name, names in MEMBERS.items():
        cls = getattr(unreal, cls_name, None)
        if cls is None:
            missing.append(cls_name)
            continue
        missing.extend(cls_name + "." + n for n in names if not hasattr(cls, n))
    for enum_name, values in ENUMS.items():
        enum = getattr(unreal, enum_name, None)
        missing.extend(enum_name + "." + v for v in values if not hasattr(enum, v))

    built = {}
    for label, make in STRUCTS.items():
        try:
            make()
            built[label] = "ok"
        except Exception as exc:  # noqa: BLE001
            built[label] = "FAIL " + str(exc)

    imports = {}
    for module in MODULES:
        try:
            __import__(module)
            imports[module] = "ok"
        except Exception:  # noqa: BLE001
            imports[module] = traceback.format_exc()

    failed_builds = [k for k, v in built.items() if v != "ok"]
    failed_imports = [k for k, v in imports.items() if v != "ok"]
    report = {
        "missing": missing, "failed_builds": failed_builds, "failed_imports": failed_imports,
        "references_checked": len(refs), "rig_control_types": [x for x in dir(unreal.RigControlType) if x.isupper()],
        "built": built, "imports": imports, "python": sys.version,
    }
    folder = os.path.dirname(OUT)
    if folder and not os.path.isdir(folder):
        os.makedirs(folder)
    with open(OUT, "w") as handle:
        json.dump(report, handle, indent=1)
    if missing or failed_builds or failed_imports:
        unreal.log_error("ANIMTK_API_CHECK FAIL: missing={} builds={} imports={} (report: {})".format(missing, failed_builds, failed_imports, OUT))
    else:
        unreal.log("ANIMTK_API_CHECK PASS: {} references, {} members, {} structs, {} modules (report: {})".format(
            len(refs), sum(len(v) for v in MEMBERS.values()), len(built), len(imports), OUT))


main()
