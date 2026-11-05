# SPDX-License-Identifier: Apache-2.0
"""SUM104 negative: loops inside a transaction, no mutator, a non-actor class, a nested def."""
import unreal

subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
with unreal.ScopedEditorTransaction("Rename"):
    for actor in subsystem.get_all_level_actors():
        actor.set_actor_label("Renamed")

for actor in subsystem.get_all_level_actors():
    print(actor.get_actor_label())

for mesh in [unreal.StaticMesh()]:
    mesh.set_editor_property("lod_group", "LargeProp")

for actor in subsystem.get_all_level_actors():
    def later(a=actor):
        a.set_actor_label("Later")
