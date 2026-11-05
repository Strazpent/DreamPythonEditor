# SPDX-License-Identifier: Apache-2.0
"""The shared docstring parser (TDD §7.4.4), as ``dream_docstrings``.

``dream_launch.py`` loads ``Content/Python/dream_editor/_impl/docstrings.py`` by path under that name, so the
``dream_editor`` package never runs (T-LSP-032). A caller that imports this package directly (the tier-2 tests) gets
the same file loaded the same way.
"""

import importlib.util
import os
import sys

MODULE_NAME = "dream_docstrings"


def docstrings_path():
    """``<plugin>/Content/Python/dream_editor/_impl/docstrings.py``, from this file's place under ``Tools/Python``."""
    here = os.path.dirname(os.path.abspath(__file__))
    plugin = os.path.normpath(os.path.join(here, "..", "..", "..", ".."))
    return os.path.join(plugin, "Content", "Python", "dream_editor", "_impl", "docstrings.py")


def load(path=None):
    """The ``dream_docstrings`` module, loaded by path on first use."""
    module = sys.modules.get(MODULE_NAME)
    if module is not None:
        return module
    spec = importlib.util.spec_from_file_location(MODULE_NAME, path or docstrings_path())
    module = importlib.util.module_from_spec(spec)
    sys.modules[MODULE_NAME] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(MODULE_NAME, None)
        raise
    return module


docstrings = load()
