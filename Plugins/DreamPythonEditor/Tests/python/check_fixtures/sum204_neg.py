# SPDX-License-Identifier: Apache-2.0
"""SUM204 negative: a shadowed input, a thread target, a method called input."""
import threading


def reader():
    return input()


class Form:
    def input(self):
        return "x"


threading.Thread(target=reader).start()
value = Form().input()


def scoped():
    input = str
    return input(3)
