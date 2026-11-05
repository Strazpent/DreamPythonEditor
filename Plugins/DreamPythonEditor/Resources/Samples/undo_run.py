# Dream sample: a run that changes the level, to try Undo. It spawns a point light and moves it;
# History > Undo Last Run (or Undo this run on the run's line in the output) takes it all back.
# SPDX-License-Identifier: Apache-2.0
import unreal

actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
light = actors.spawn_actor_from_class(unreal.PointLight, unreal.Vector(0.0, 0.0, 200.0))
light.set_actor_label("Dream sample light")
for step in range(1, 4):
    light.set_actor_location(unreal.Vector(100.0 * step, 0.0, 200.0), False, False)
print(f"{light.get_actor_label()} is at {light.get_actor_location()}")
