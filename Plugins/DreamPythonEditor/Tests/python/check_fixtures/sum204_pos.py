# SPDX-License-Identifier: Apache-2.0
"""SUM204 positive: the builtin input() on the game thread."""
name = input("Name? ")  # expect: SUM204


def ask():
    return input()  # expect: SUM204
