# SPDX-License-Identifier: Apache-2.0
"""SUM205 negative: joins with a timeout, str.join, a queue."""
import queue
import threading

thread = threading.Thread(target=print)
thread.join(5)
thread.join(timeout=1.0)
text = ",".join(["a", "b"])
jobs = queue.Queue()
jobs.join()
