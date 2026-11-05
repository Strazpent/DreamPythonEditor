# SPDX-License-Identifier: Apache-2.0
"""SUM201 positive: unreal in a direct target, a depth-2 helper, Thread.run, an executor, a lambda."""
import threading
from concurrent.futures import ThreadPoolExecutor

import unreal


def worker():
    unreal.log("from a thread")  # expect: SUM201


def helper():
    return unreal.EditorAssetLibrary.list_assets("/Game")  # expect: SUM201


def outer():
    helper()


class Loader(threading.Thread):
    def run(self):
        unreal.EditorAssetLibrary.load_asset("/Game/A")  # expect: SUM201


def submitted(path):
    return unreal.EditorAssetLibrary.does_asset_exist(path)  # expect: SUM201


threading.Thread(target=worker).start()
threading.Thread(None, outer).start()
Loader().start()
with ThreadPoolExecutor() as pool:
    pool.submit(submitted, "/Game/B")
threading.Thread(target=lambda: unreal.log("lambda")).start()  # expect: SUM201
