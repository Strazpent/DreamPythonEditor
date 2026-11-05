# SPDX-License-Identifier: Apache-2.0
"""The one entry point of Dream's out-of-process tools (TDD §2.2, §7.3.3 Launch).

    python.exe -I <plugin>/Tools/Python/dream_launch.py check --data <snapshot dir> --parent-pid <pid> [--key <key>]

In 3.11 ``-I`` implies ``-P``, so the script's folder is not on ``sys.path``: this adds ``Tools/Python``. The shared
docstring parser is loaded by path as ``dream_docstrings`` (``spec_from_file_location``), so ``dream_editor/__init__``
never runs here (T-LSP-032). Standard library only.
"""

import sys

# Recorded before anything of ours is imported: the import audit (T-LSP-032) checks only what Dream added.
STARTUP_MODULES = sorted(sys.modules)
# No __pycache__ next to the plugin's sources: an engine or Fab install may be read-only, and a packaged plugin must
# not carry bytecode (STATUS known risks).
sys.dont_write_bytecode = True

import importlib.util  # noqa: E402
import os  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.normpath(os.path.join(HERE, "..", ".."))
DOCSTRINGS = os.path.join(PLUGIN, "Content", "Python", "dream_editor", "_impl", "docstrings.py")
USAGE = "usage: dream_launch.py check --data <snapshot dir> --parent-pid <pid> [--key <key>]"


def load_docstrings(path=DOCSTRINGS):
    """``dream_editor/_impl/docstrings.py`` as the module ``dream_docstrings``."""
    module = sys.modules.get("dream_docstrings")
    if module is not None:
        return module
    spec = importlib.util.spec_from_file_location("dream_docstrings", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules["dream_docstrings"] = module
    spec.loader.exec_module(module)
    return module


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        sys.stderr.write(USAGE + "\n")
        return 2 if not argv else 0
    command, rest = argv[0], argv[1:]
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    if command == "check":
        load_docstrings()
        from dream_tools.check import server
        return server.main(rest, startup_modules=STARTUP_MODULES)
    sys.stderr.write("dream_launch.py: unknown command %r\n%s\n" % (command, USAGE))
    return 2


if __name__ == "__main__":
    code = main(sys.argv[1:])
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.flush()
        except (OSError, ValueError):
            pass
    # os._exit: the reader thread may still be blocked in a read of stdin, and a normal interpreter exit could wait
    # on that stream's lock.
    os._exit(code if isinstance(code, int) else 1)
