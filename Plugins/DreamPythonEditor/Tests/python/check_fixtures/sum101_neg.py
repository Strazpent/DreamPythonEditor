# SPDX-License-Identifier: Apache-2.0
"""SUM101 negative: known names, inherited members, getattr and names assigned into unreal."""
import os
import unreal
import unreal as ue
from unreal import EditorAssetLibrary, StaticMesh as SM

unreal.MyInjectedHelper = object()
setattr(unreal, "OtherHelper", 1)
helper = unreal.MyInjectedHelper
other = unreal.OtherHelper
dynamic = getattr(unreal, "Whatever")
label = unreal.StaticMeshActor.get_actor_label
cast = unreal.Object.cast
cdo = unreal.StaticMesh.get_default_object()
mro = unreal.Actor.mro()
module_name = unreal.__name__
channel = unreal.CollisionChannel.ECC_PAWN
zero = unreal.Vector.ZERO
unreal.log("hello")
data = unreal.DREAM_TEST_DATA
asset = ue.EditorAssetLibrary.load_asset("/Game/A")
lods = SM.get_num_lods
listed = EditorAssetLibrary.list_assets("/Game")
path = os.EditorAssetLibary
