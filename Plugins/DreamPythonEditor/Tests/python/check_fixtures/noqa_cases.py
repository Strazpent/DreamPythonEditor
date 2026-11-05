# SPDX-License-Identifier: Apache-2.0
"""# noqa: a code list, a bare noqa, another code, a noqa inside a string."""
import unreal

first = unreal.EditorAssetLibary  # noqa: SUM101
second = unreal.EditorAssetLibary  # noqa
third = unreal.EditorAssetLibary  # noqa: SUM102  # expect: SUM101
fourth = (unreal.EditorAssetLibary, "# noqa")  # expect: SUM101
fifth = unreal.EditorAssetLibary  # NOQA:SUM101,SUM103
