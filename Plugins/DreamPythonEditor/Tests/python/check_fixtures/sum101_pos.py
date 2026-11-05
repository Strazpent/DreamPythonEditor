# SPDX-License-Identifier: Apache-2.0
"""SUM101 positive: unknown unreal.* names at level 1 and level 2, through every import form."""
import unreal
import unreal as ue
from unreal import EditorAssetLibary  # expect: SUM101
from unreal import StaticMesh as SM

asset = unreal.EditorAssetLibary.load_asset("/Game/A")  # expect: SUM101
actor_class = ue.StaticMeshActr  # expect: SUM101
loaded = unreal.EditorAssetLibrary.lod_asset("/Game/A")  # expect: SUM101
lods = SM.get_num_lodz  # expect: SUM101
