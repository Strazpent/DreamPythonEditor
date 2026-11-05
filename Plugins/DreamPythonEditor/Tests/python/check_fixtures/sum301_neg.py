# SPDX-License-Identifier: Apache-2.0
"""SUM301 negative: temp and saved folders, other functions."""
import os
import shutil
import tempfile

import unreal

temp = tempfile.gettempdir()
os.rename(os.path.join(temp, "a.txt"), os.path.join(temp, "b.txt"))
shutil.move(unreal.Paths.project_saved_dir() + "Logs/a.log", "C:/Backup/a.log")
os.replace("C:/Projects/Game/Content/A.uasset", "C:/Projects/Game/Content/B.uasset")
unreal.EditorAssetLibrary.rename_asset("/Game/A", "/Game/B")
