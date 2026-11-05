# SPDX-License-Identifier: Apache-2.0
"""SUM103 positive: renamed members and types, a deprecated class, a renamed module function."""
import unreal
from unreal import EditorLevelUtils  # expect: SUM103

actor = unreal.StaticMeshActor()
actor.k2_set_actor_location(unreal.Vector(), False, False)  # expect: SUM103
utils = unreal.EditorLevelUtils  # expect: SUM103
actors = unreal.EditorLevelLibrary.get_all_level_actors()  # expect: SUM103
all_actors = unreal.get_level_actors_old()  # expect: SUM103
