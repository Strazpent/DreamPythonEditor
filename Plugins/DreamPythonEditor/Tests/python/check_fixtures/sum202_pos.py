# SPDX-License-Identifier: Apache-2.0
"""SUM202 positive: time.sleep and an imported sleep in loops on the game thread."""
import time
from time import sleep

for step in range(10):
    time.sleep(0.5)  # expect: SUM202

while True:
    sleep(1)  # expect: SUM202
    break
