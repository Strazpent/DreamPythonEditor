# SPDX-License-Identifier: Apache-2.0
"""SUM201 negative: thread targets without unreal; unreal on the game thread."""
import threading

import unreal


def worker(results):
    results.append(sum(range(10)))


def uses_unreal():
    return unreal.EditorAssetLibrary.list_assets("/Game")


results = []
threading.Thread(target=worker, args=(results,)).start()
assets = uses_unreal()
