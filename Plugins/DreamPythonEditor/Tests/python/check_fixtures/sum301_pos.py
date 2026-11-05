# SPDX-License-Identifier: Apache-2.0
"""SUM301 positive: os.rename and shutil.move on Content paths."""
import os
import shutil
from pathlib import Path

import unreal

root = "C:/Projects/Game"
name = "Rock.uasset"
os.rename("C:/Projects/Game/Content/Rock.uasset", "C:/Projects/Game/Content/Stone.uasset")  # expect: SUM301
shutil.move(f"{root}/Content/{name}", f"{root}/Content/Old/{name}")  # expect: SUM301
os.rename(os.path.join(unreal.Paths.project_content_dir(), name), "x")  # expect: SUM301
source = unreal.SystemLibrary.get_project_content_directory() + name
os.rename(source, "y")  # expect: SUM301
shutil.move("/Game/Maps/Level", "/Game/Maps/Old")  # expect: SUM301
os.rename(Path(root) / "Content" / name, "z")  # expect: SUM301
os.rename("D:\\Projects\\Game\\Content\\Rock.uasset", "w")  # expect: SUM301
