# SPDX-License-Identifier: Apache-2.0
"""SUM102 negative: Read-Write attributes, self, set_editor_property, unknown names that are attributes somewhere."""
import unreal

tex = unreal.Texture2D()
tex.lod_bias = 1
static_mesh = unreal.StaticMesh()
static_mesh.light_map_coordinate_index = 1
static_mesh.set_editor_property("lod_group", "LargeProp")
static_mesh.custom_python_attribute = 3


def anything(thing):
    thing.lod_group = 2


class Spawner:
    def __init__(self):
        self.initial_life_span = 1.0
