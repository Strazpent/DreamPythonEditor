# SPDX-License-Identifier: Apache-2.0
"""SUM203 positive: event loops run on the game thread."""
import asyncio


async def main():
    await asyncio.sleep(0)


asyncio.run(main())  # expect: SUM203
loop = asyncio.new_event_loop()
loop.run_until_complete(main())  # expect: SUM203
loop.run_forever()  # expect: SUM203
