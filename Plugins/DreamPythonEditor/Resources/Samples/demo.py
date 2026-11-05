# Dream sample: the demo from the screenshots. Run it all at once, or one line at a time
# (Run Line and Advance), and watch the output; the last line's value shows as its result.
# SPDX-License-Identifier: Apache-2.0
import math

for i in range(3):
    print(f"square of {i} is {i * i}")


def circle_area(r):
    return math.pi * r ** 2


circle_area(2.0)
