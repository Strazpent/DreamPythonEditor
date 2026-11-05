# SPDX-License-Identifier: Apache-2.0
"""SUM205 positive: Thread.join() with no timeout."""
import threading
from threading import Thread


def work():
    pass


thread = threading.Thread(target=work)
thread.start()
thread.join()  # expect: SUM205


class Worker(Thread):
    def run(self):
        pass


worker = Worker()
worker.join()  # expect: SUM205
