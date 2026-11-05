# Dream sample: how the output shows each kind of line: a print, a Python warning, Unreal's
# log warning and log error, a multi-line result, and (commented out) an exception to try.
# SPDX-License-Identifier: Apache-2.0
import warnings

import unreal

print("a plain print shows as it is")
warnings.warn("a Python warning shows as a warning block")
unreal.log_warning("unreal.log_warning shows as a warning")
unreal.log_error("unreal.log_error shows as an error; the run still ends OK")


class Report:
    def __repr__(self):
        return "Report(\n    items=3,\n    status='ok',\n)"


Report()

# Remove the # on the next line to see a traceback whose lines link back to this file:
# raise ValueError("an exception ends the run with a traceback")
