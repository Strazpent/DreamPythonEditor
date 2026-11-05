# SPDX-License-Identifier: Apache-2.0
"""SUM202 negative: no loop, a thread target, a task generator, a def inside a loop."""
import threading
import time

import dream_editor

time.sleep(0.1)


def poll():
    for _ in range(3):
        time.sleep(1)


@dream_editor.task
def steps():
    for _ in range(3):
        time.sleep(1)
        yield


for index in range(3):
    def later():
        time.sleep(1)

threading.Thread(target=poll).start()
