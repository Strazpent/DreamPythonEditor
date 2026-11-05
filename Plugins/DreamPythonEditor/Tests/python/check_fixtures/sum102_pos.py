# SPDX-License-Identifier: Apache-2.0
"""SUM102 positive: edit-only, BlueprintReadOnly-but-editable, AugAssign and the unknown-class information."""
import unreal

mesh = unreal.EditorAssetLibrary.load_asset("/Game/Meshes/Rock")
static_mesh: unreal.StaticMesh = mesh
static_mesh.lod_group = "LargeProp"  # expect: SUM102
static_mesh.light_map_resolution = 128  # expect: SUM102
static_mesh.light_map_resolution += 64  # expect: SUM102


def configure(actor):
    actor.initial_life_span = 5.0  # expect: SUM102


def by_guard(thing):
    if isinstance(thing, unreal.StaticMesh):
        thing.lod_group = "SmallProp"  # expect: SUM102
