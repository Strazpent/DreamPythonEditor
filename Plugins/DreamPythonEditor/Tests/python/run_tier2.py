# SPDX-License-Identifier: Apache-2.0
"""Tier 2 runner (TDD §14.1): unittest discovery over Tests/python/test_*.py under the engine's python.exe -I -B.

``-I`` leaves the script directory off sys.path, so this inserts Tests/python and the plugin's Content/Python.
Usage: python.exe -I -B run_tier2.py [-v] [-k pattern]
"""

import argparse
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
CONTENT_PYTHON = os.path.normpath(os.path.join(HERE, "..", "..", "Content", "Python"))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("-k", dest="patterns", action="append", default=None,
                        help="only tests whose name contains this substring")
    args = parser.parse_args(argv)
    for path in (CONTENT_PYTHON, HERE):
        if path not in sys.path:
            sys.path.insert(0, path)
    loader = unittest.TestLoader()
    if args.patterns:
        loader.testNamePatterns = [f"*{pattern}*" for pattern in args.patterns]
    suite = loader.discover(start_dir=HERE, pattern="test_*.py", top_level_dir=HERE)
    print(f"Python {sys.version.split()[0]} at {sys.executable}", flush=True)
    result = unittest.TextTestRunner(verbosity=2 if args.verbose else 1, stream=sys.__stderr__).run(suite)
    print(f"tier 2: {result.testsRun} run, {len(result.failures)} failures, {len(result.errors)} errors, "
          f"{len(result.skipped)} skipped", flush=True)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
