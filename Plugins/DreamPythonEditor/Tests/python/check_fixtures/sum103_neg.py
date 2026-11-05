# SPDX-License-Identifier: Apache-2.0
"""SUM103 negative: the new names."""
import unreal

actor = unreal.StaticMeshActor()
actor.set_actor_location(unreal.Vector(), False, False)
utils = unreal.LevelUtils
asset = unreal.EditorAssetLibrary.load_asset("/Game/A")
unreal.log("a module function that is not deprecated")
