# Dream sample: a long run with a progress bar, to try Stop. While it runs, press Stop in the
# Run window (or Esc) and it ends at once; the editor stays live the whole time.
# SPDX-License-Identifier: Apache-2.0
import time

import dream_editor

STEPS = 50
for i in range(STEPS):
    dream_editor.progress(i, STEPS, f"item {i}")
    print(f"step {i:02d}")
    time.sleep(0.1)
print("long run done")
