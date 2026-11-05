# Dream sample: cells. Each "# %%" line starts a cell, and the Structure panel lists the cells;
# select one cell's lines and run the selection to run just that cell.
# SPDX-License-Identifier: Apache-2.0

# %% Setup
import math

values = [1.0, 2.0, 3.0]

# %% Compute
squares = [v * v for v in values]
print("squares:", squares)

# %% Report
total = math.fsum(squares)
print(f"sum of squares: {total}")
