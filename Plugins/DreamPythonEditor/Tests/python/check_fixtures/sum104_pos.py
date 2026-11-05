# SPDX-License-Identifier: Apache-2.0
"""SUM104 positive (One undo step per run off): actor edits in loops without a transaction."""
import unreal

subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
for actor in subsystem.get_all_level_actors():  # expect: SUM104
    actor.set_actor_label("Renamed")

selected = subsystem.get_selected_level_actors()
while selected:  # expect: SUM104
    subsystem.destroy_actor(selected.pop())


def move(actors: list[unreal.StaticMeshActor]):
    for actor in actors:  # expect: SUM104
        for _ in range(2):
            actor.static_mesh_component.set_world_location(unreal.Vector(), False, False)
