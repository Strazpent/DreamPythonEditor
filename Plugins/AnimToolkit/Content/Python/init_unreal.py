# SPDX-License-Identifier: Apache-2.0
# Runs when the editor starts (UE runs every init_unreal.py on the Python path).
try:
    import animtk.startup
    animtk.startup.run()
except Exception:  # noqa: BLE001 - never block the editor start
    import traceback
    import unreal
    unreal.log_error("[AnimToolkit] start-up failed:\n" + traceback.format_exc())
