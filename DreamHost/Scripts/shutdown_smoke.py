# SPDX-License-Identifier: Apache-2.0
# Normal-exit smoke (run_smoke.ps1): runs through Dream.RunFile, then asks the editor for a normal exit, so OnPreExit
# and the module's ShutdownModule run (an automation Quit force-exits and skips them).
import unreal

print("dream-shutdown-smoke: ran inside Dream Python Editor")
unreal.log("dream-shutdown-smoke: requesting a normal editor exit")
unreal.SystemLibrary.quit_editor()
