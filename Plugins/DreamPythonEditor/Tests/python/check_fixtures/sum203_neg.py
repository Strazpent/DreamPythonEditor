# SPDX-License-Identifier: Apache-2.0
"""SUM203 negative: the loop runs in a thread target."""
import asyncio
import threading


async def main():
    await asyncio.sleep(0)


def runner():
    asyncio.run(main())


threading.Thread(target=runner).start()
coroutine = asyncio.sleep(1)
